/*
 * HID parsing library
 *
 * Copyright 2021 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"

#include <ddk/wdm.h>
#include <ddk/hidpddi.h>

#include "wine/hid.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(hid);

/* The report-descriptor parser (parse_descriptor) is implemented in Rust
 * (rust/src_sys/hidparse_pe): it walks the untrusted, device-supplied descriptor
 * with bounds-checked slicing -- removing the off-by-one / pointer-runaway the C
 * item loop had on malformed input -- and returns a pool-allocated
 * struct hid_preparsed_data the caller frees with ExFreePool, exactly like the C
 * parse_descriptor it replaces. */
extern struct hid_preparsed_data *rs_hid_parse_descriptor( const BYTE *descriptor,
                                                           unsigned int length,
                                                           POOL_TYPE pool_type );

enum
{
    TAG_TYPE_MAIN = 0x0,
    TAG_TYPE_GLOBAL,
    TAG_TYPE_LOCAL,
    TAG_TYPE_RESERVED,
};

enum
{
    TAG_MAIN_INPUT = 0x08,
    TAG_MAIN_OUTPUT = 0x09,
    TAG_MAIN_FEATURE = 0x0B,
    TAG_MAIN_COLLECTION = 0x0A,
    TAG_MAIN_END_COLLECTION = 0x0C
};

static inline const char *debugstr_hid_value_caps( struct hid_value_caps *caps )
{
    if (!caps) return "(null)";
    return wine_dbg_sprintf( "RId %d, Usg %02x:%02x-%02x Dat %02x-%02x, Str %d-%d, Des %d-%d, "
                             "Bits %02lx Flags %#lx, LCol %d LUsg %02x:%02x, BitSz %d, RCnt %d, Unit %lx E%+ld, Log %+ld-%+ld, Phy %+ld-%+ld",
                             caps->report_id, caps->usage_page, caps->usage_min, caps->usage_max, caps->data_index_min, caps->data_index_max,
                             caps->string_min, caps->string_max, caps->designator_min, caps->designator_max, caps->bit_field, caps->flags,
                             caps->link_collection, caps->link_usage_page, caps->link_usage, caps->bit_size, caps->report_count,
                             caps->units, caps->units_exp, caps->logical_min, caps->logical_max, caps->physical_min, caps->physical_max );
}

static inline const char *debugstr_hid_collection_node( struct hid_collection_node *node )
{
    if (!node) return "(null)";
    return wine_dbg_sprintf( "Usg %02x:%02x, Parent %u, Next %u, NbChild %u, Child %u, Type %02lx",
                             node->usage_page, node->usage, node->parent, node->next_sibling,
                             node->number_of_children, node->first_child, node->collection_type );
}

static void debug_print_preparsed( struct hid_preparsed_data *data )
{
    unsigned int i, end;

    if (TRACE_ON(hid))
    {
        TRACE( "usage %02x:%02x input %u-(%u)-%u, report len %u output %u-(%u)-%u, report len %u "
               "feature %u-(%u)-%u, report len %u collections %u\n", data->usage_page, data->usage,
                data->input_caps_start, data->input_caps_count, data->input_caps_end, data->input_report_byte_length,
                data->output_caps_start, data->output_caps_count, data->output_caps_end, data->output_report_byte_length,
                data->feature_caps_start, data->feature_caps_count, data->feature_caps_end, data->feature_report_byte_length,
                data->number_link_collection_nodes );
        end = data->input_caps_end - data->input_caps_start;
        for (i = 0; i < end; i++) TRACE( "input %d: %s\n", i, debugstr_hid_value_caps( HID_INPUT_VALUE_CAPS( data ) + i ) );
        end = data->output_caps_end - data->output_caps_start;
        for (i = 0; i < end; i++) TRACE( "output %d: %s\n", i, debugstr_hid_value_caps( HID_OUTPUT_VALUE_CAPS( data ) + i ) );
        end = data->feature_caps_end - data->feature_caps_start;
        for (i = 0; i < end; i++) TRACE( "feature %d: %s\n", i, debugstr_hid_value_caps( HID_FEATURE_VALUE_CAPS( data ) + i ) );
        end = data->number_link_collection_nodes;
        for (i = 0; i < end; i++) TRACE( "collection %d: %s\n", i, debugstr_hid_collection_node( HID_COLLECTION_NODES( data ) + i ) );
    }
}

struct hid_preparsed_data *parse_descriptor( BYTE *descriptor, unsigned int length, POOL_TYPE pool_type )
{
    struct hid_preparsed_data *data;
    int i;

    if (TRACE_ON(hid))
    {
        TRACE( "descriptor %p, length %u:\n", descriptor, length );
        for (i = 0; i < length;)
        {
            TRACE( "%08x ", i );
            do { TRACE( " %02x", descriptor[i] ); } while (++i % 16 && i < length);
            TRACE( "\n" );
        }
    }

    if ((data = rs_hid_parse_descriptor( descriptor, length, pool_type ))) debug_print_preparsed( data );
    return data;
}

static BYTE **parse_top_level_collections( BYTE *descriptor, unsigned int length, ULONG *count )
{
    BYTE *ptr, *end, **tmp, **tlcs;
    UINT32 size, depth = 0;

    if (!(tlcs = malloc( sizeof(*tlcs) ))) return NULL;
    tlcs[0] = descriptor;
    *count = 0;

    for (ptr = descriptor, end = descriptor + length; ptr != end; ptr += size + 1)
    {
        size = (*ptr & 0x03);
        if (size == 3) size = 4;
        if (ptr + size > end)
        {
            ERR( "Need %d bytes to read item value\n", size );
            break;
        }

#define SHORT_ITEM( tag, type ) (((tag) << 4) | ((type) << 2))
        switch (*ptr & SHORT_ITEM( 0xf, 0x3 ))
        {
        case SHORT_ITEM( TAG_MAIN_COLLECTION, TAG_TYPE_MAIN ):
            if (depth++) break;
            break;
        case SHORT_ITEM( TAG_MAIN_END_COLLECTION, TAG_TYPE_MAIN ):
            if (--depth) break;
            *count = *count + 1;
            if (!(tmp = realloc( tlcs, (*count + 1) * sizeof(*tlcs) )))
            {
                ERR( "Failed to allocate memory for TLCs\n" );
                return tlcs;
            }
            tlcs = tmp;
            tlcs[*count] = ptr + size + 1;
            break;
        }
#undef SHORT_ITEM
    }

    TRACE( "Found %lu TLCs\n", *count );
    return tlcs;
}

NTSTATUS WINAPI HidP_GetCollectionDescription( PHIDP_REPORT_DESCRIPTOR report_desc, ULONG report_desc_len,
                                               POOL_TYPE pool_type, HIDP_DEVICE_DESC *device_desc )
{
    ULONG i, len, tlc_count, report_count = 0, input_len[256] = {0}, output_len[256] = {0}, feature_len[256] = {0}, collection[256] = {0};
    struct hid_value_caps *caps, *caps_end;
    struct hid_preparsed_data *preparsed;
    BYTE **tlcs;

    TRACE( "report_desc %p, report_desc_len %lu, pool_type %u, device_desc %p.\n",
            report_desc, report_desc_len, pool_type, device_desc );

    memset( device_desc, 0, sizeof(*device_desc) );

    if (!(tlcs = parse_top_level_collections( report_desc, report_desc_len, &tlc_count ))) goto failed;

    len = sizeof(*device_desc->CollectionDesc) * tlc_count;
    if (!(device_desc->CollectionDesc = ExAllocatePool( pool_type, len ))) goto failed;

    for (i = 0; i < tlc_count; ++i)
    {
        TRACE( "parsing TLC %lu, start %p, end %p\n", i, tlcs[i], tlcs[i + 1] );

        if (!(preparsed = parse_descriptor( tlcs[i], tlcs[i + 1] - tlcs[i], pool_type ))) goto failed;

        len = preparsed->caps_size + FIELD_OFFSET(struct hid_preparsed_data, value_caps[0]) +
              preparsed->number_link_collection_nodes * sizeof(struct hid_collection_node);

        device_desc->CollectionDescLength++;
        device_desc->CollectionDesc[i].UsagePage = preparsed->usage_page;
        device_desc->CollectionDesc[i].Usage = preparsed->usage;
        device_desc->CollectionDesc[i].CollectionNumber = i + 1;
        device_desc->CollectionDesc[i].InputLength = preparsed->input_report_byte_length;
        device_desc->CollectionDesc[i].OutputLength = preparsed->output_report_byte_length;
        device_desc->CollectionDesc[i].FeatureLength = preparsed->feature_report_byte_length;
        device_desc->CollectionDesc[i].PreparsedDataLength = len;
        device_desc->CollectionDesc[i].PreparsedData = (PHIDP_PREPARSED_DATA)preparsed;

        caps = HID_INPUT_VALUE_CAPS( preparsed );
        caps_end = caps + preparsed->input_caps_end - preparsed->input_caps_start;
        for (; caps != caps_end; ++caps)
        {
            len = caps->start_byte * 8 + caps->start_bit + caps->bit_size * caps->report_count;
            if (!input_len[caps->report_id]) report_count++;
            input_len[caps->report_id] = max(input_len[caps->report_id], len);
            collection[caps->report_id] = i;
        }

        caps = HID_OUTPUT_VALUE_CAPS( preparsed );
        caps_end = caps + preparsed->output_caps_end - preparsed->output_caps_start;
        for (; caps != caps_end; ++caps)
        {
            len = caps->start_byte * 8 + caps->start_bit + caps->bit_size * caps->report_count;
            if (!input_len[caps->report_id] && !output_len[caps->report_id]) report_count++;
            output_len[caps->report_id] = max(output_len[caps->report_id], len);
            collection[caps->report_id] = i;
        }

        caps = HID_FEATURE_VALUE_CAPS( preparsed );
        caps_end = caps + preparsed->feature_caps_end - preparsed->feature_caps_start;
        for (; caps != caps_end; ++caps)
        {
            len = caps->start_byte * 8 + caps->start_bit + caps->bit_size * caps->report_count;
            if (!input_len[caps->report_id] && !output_len[caps->report_id] && !feature_len[caps->report_id]) report_count++;
            feature_len[caps->report_id] = max(feature_len[caps->report_id], len);
            collection[caps->report_id] = i;
        }
    }

    len = sizeof(*device_desc->ReportIDs) * report_count;
    if (!(device_desc->ReportIDs = ExAllocatePool( pool_type, len ))) goto failed;

    for (i = 0, report_count = 0; i < 256; ++i)
    {
        if (!input_len[i] && !output_len[i] && !feature_len[i]) continue;
        device_desc->ReportIDs[report_count].ReportID = i;
        device_desc->ReportIDs[report_count].CollectionNumber = collection[i] + 1;
        device_desc->ReportIDs[report_count].InputLength = (input_len[i] + 7) / 8;
        device_desc->ReportIDs[report_count].OutputLength = (output_len[i] + 7) / 8;
        device_desc->ReportIDs[report_count].FeatureLength = (feature_len[i] + 7) / 8;
        report_count++;
    }
    device_desc->ReportIDsLength = report_count;

    free( tlcs );
    return HIDP_STATUS_SUCCESS;

failed:
    if (device_desc->CollectionDesc)
    {
        for (i = 0; i < device_desc->CollectionDescLength; ++i)
            ExFreePool( device_desc->CollectionDesc[i].PreparsedData );
        ExFreePool( device_desc->CollectionDesc );
    }
    free( tlcs );
    return HIDP_STATUS_INTERNAL_ERROR;
}

void WINAPI HidP_FreeCollectionDescription( HIDP_DEVICE_DESC *device_desc )
{
    UINT i;

    TRACE( "device_desc %p.\n", device_desc );

    for (i = 0; i < device_desc->CollectionDescLength; ++i)
        ExFreePool( device_desc->CollectionDesc[i].PreparsedData );
    ExFreePool( device_desc->CollectionDesc );
    ExFreePool( device_desc->ReportIDs );
}

NTSTATUS WINAPI DriverEntry( DRIVER_OBJECT *driver, UNICODE_STRING *path )
{
    return STATUS_SUCCESS;
}
