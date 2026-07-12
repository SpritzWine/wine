/*
 * Human Input Devices
 *
 * Copyright (C) 2015 Aric Stewart
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"

#include "hidusage.h"
#include "ddk/hidpi.h"
#include "wine/hid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(hid);

/* The whole HidP_* preparsed-data engine -- the caps walk, the bit-field read/
 * write over the untrusted HID report, and the report-descriptor-derived struct
 * emitters -- lives in Rust (rust/src_dll/hid_pe -> libwine_hid_pe.a, linked via
 * EXTRADLLFLAGS). It replaces a hand-rolled C pointer walk + a bit copier that
 * indexed the live report by untrusted caps offsets with no bound against the
 * report length (an OOB read, and on the Set path an OOB write). Every function
 * below is a thin marshalling shim; hidp.c carries no HID logic of its own. The
 * one exception is the still-stubbed TranslateUsagesToI8042ScanCodes.
 *
 * hidp.c is shared with hidparse.sys (PARENTSRC=../hid), so both modules link
 * libwine_hid_pe.a; see the Makefile.in of each. */

extern NTSTATUS rs_hidp_get_caps( const void *preparsed, void *caps );
extern NTSTATUS rs_hidp_get_specific_button_caps( UINT report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                                  void *caps, USHORT *caps_count, const void *preparsed );
extern NTSTATUS rs_hidp_get_specific_value_caps( UINT report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                                 void *caps, USHORT *caps_count, const void *preparsed );
extern NTSTATUS rs_hidp_get_link_collection_nodes( void *nodes, ULONG *nodes_len, const void *preparsed );
extern NTSTATUS rs_hidp_initialize_report_for_id( UINT report_type, UCHAR report_id, const void *preparsed,
                                                  void *report, ULONG report_len );
extern ULONG rs_hidp_max_usage_list_length( UINT report_type, USAGE usage_page, const void *preparsed );
extern ULONG rs_hidp_max_data_list_length( UINT report_type, const void *preparsed );

extern NTSTATUS rs_hidp_get_usage_value( UINT report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                         ULONG *value_out, const void *preparsed, const void *report, ULONG report_len );
extern NTSTATUS rs_hidp_get_scaled_usage_value( UINT report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                                LONG *value_out, const void *preparsed, const void *report, ULONG report_len );
extern NTSTATUS rs_hidp_get_usage_value_array( UINT report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                               void *value_buf, USHORT value_len, const void *preparsed,
                                               const void *report, ULONG report_len );
extern NTSTATUS rs_hidp_set_usage_value( UINT report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                         ULONG value, const void *preparsed, void *report, ULONG report_len );
extern NTSTATUS rs_hidp_set_scaled_usage_value( UINT report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                                LONG value, const void *preparsed, void *report, ULONG report_len );
extern NTSTATUS rs_hidp_set_usage_value_array( UINT report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                               const void *value_buf, USHORT value_len, const void *preparsed,
                                               void *report, ULONG report_len );

extern NTSTATUS rs_hidp_get_usages( UINT report_type, USAGE usage_page, USHORT collection, USAGE *usages,
                                    ULONG *usages_len, const void *preparsed, const void *report, ULONG report_len );
extern NTSTATUS rs_hidp_get_usages_ex( UINT report_type, USHORT collection, void *usages, ULONG *usages_len,
                                       const void *preparsed, const void *report, ULONG report_len );
extern NTSTATUS rs_hidp_set_usages( UINT report_type, USAGE usage_page, USHORT collection, const USAGE *usages,
                                    ULONG *usage_count, const void *preparsed, void *report, ULONG report_len );
extern NTSTATUS rs_hidp_unset_usages( UINT report_type, USAGE usage_page, USHORT collection, const USAGE *usages,
                                      ULONG *usage_count, const void *preparsed, void *report, ULONG report_len );
extern NTSTATUS rs_hidp_get_data( UINT report_type, void *data, ULONG *data_len, const void *preparsed,
                                  const void *report, ULONG report_len );
extern NTSTATUS rs_hidp_set_data( UINT report_type, void *data, ULONG *data_len, const void *preparsed,
                                  void *report, ULONG report_len );

NTSTATUS WINAPI HidP_GetCaps( PHIDP_PREPARSED_DATA preparsed_data, HIDP_CAPS *caps )
{
    TRACE( "preparsed_data %p, caps %p.\n", preparsed_data, caps );
    return rs_hidp_get_caps( preparsed_data, caps );
}

NTSTATUS WINAPI HidP_GetSpecificButtonCaps( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection,
                                            USAGE usage, HIDP_BUTTON_CAPS *caps, USHORT *caps_count,
                                            PHIDP_PREPARSED_DATA preparsed_data )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usage %u, caps %p, caps_count %p, preparsed_data %p.\n",
           report_type, usage_page, collection, usage, caps, caps_count, preparsed_data );
    return rs_hidp_get_specific_button_caps( report_type, usage_page, collection, usage, caps, caps_count, preparsed_data );
}

NTSTATUS WINAPI HidP_GetButtonCaps( HIDP_REPORT_TYPE report_type, HIDP_BUTTON_CAPS *caps, USHORT *caps_count,
                                    PHIDP_PREPARSED_DATA preparsed_data )
{
    return HidP_GetSpecificButtonCaps( report_type, 0, 0, 0, caps, caps_count, preparsed_data );
}

NTSTATUS WINAPI HidP_GetSpecificValueCaps( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection,
                                           USAGE usage, HIDP_VALUE_CAPS *caps, USHORT *caps_count,
                                           PHIDP_PREPARSED_DATA preparsed_data )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usage %u, caps %p, caps_count %p, preparsed_data %p.\n",
           report_type, usage_page, collection, usage, caps, caps_count, preparsed_data );
    return rs_hidp_get_specific_value_caps( report_type, usage_page, collection, usage, caps, caps_count, preparsed_data );
}

NTSTATUS WINAPI HidP_GetValueCaps( HIDP_REPORT_TYPE report_type, HIDP_VALUE_CAPS *caps, USHORT *caps_count,
                                   PHIDP_PREPARSED_DATA preparsed_data )
{
    return HidP_GetSpecificValueCaps( report_type, 0, 0, 0, caps, caps_count, preparsed_data );
}

NTSTATUS WINAPI HidP_GetLinkCollectionNodes( HIDP_LINK_COLLECTION_NODE *nodes, ULONG *nodes_len, PHIDP_PREPARSED_DATA preparsed_data )
{
    TRACE( "nodes %p, nodes_len %p, preparsed_data %p.\n", nodes, nodes_len, preparsed_data );
    return rs_hidp_get_link_collection_nodes( nodes, nodes_len, preparsed_data );
}

NTSTATUS WINAPI HidP_InitializeReportForID( HIDP_REPORT_TYPE report_type, UCHAR report_id,
                                            PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, report_id %u, preparsed_data %p, report_buf %p, report_len %lu.\n", report_type,
           report_id, preparsed_data, report_buf, report_len );
    return rs_hidp_initialize_report_for_id( report_type, report_id, preparsed_data, report_buf, report_len );
}

ULONG WINAPI HidP_MaxUsageListLength( HIDP_REPORT_TYPE report_type, USAGE usage_page, PHIDP_PREPARSED_DATA preparsed_data )
{
    TRACE( "report_type %d, usage_page %u, preparsed_data %p.\n", report_type, usage_page, preparsed_data );
    return rs_hidp_max_usage_list_length( report_type, usage_page, preparsed_data );
}

ULONG WINAPI HidP_MaxDataListLength( HIDP_REPORT_TYPE report_type, PHIDP_PREPARSED_DATA preparsed_data )
{
    TRACE( "report_type %d, preparsed_data %p.\n", report_type, preparsed_data );
    return rs_hidp_max_data_list_length( report_type, preparsed_data );
}

NTSTATUS WINAPI HidP_GetUsageValue( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                    ULONG *value, PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usage %u, value %p, preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usage, value, preparsed_data, report_buf, report_len );
    return rs_hidp_get_usage_value( report_type, usage_page, collection, usage, value, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_GetScaledUsageValue( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection,
                                          USAGE usage, LONG *value, PHIDP_PREPARSED_DATA preparsed_data,
                                          char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usage %u, value %p, preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usage, value, preparsed_data, report_buf, report_len );
    return rs_hidp_get_scaled_usage_value( report_type, usage_page, collection, usage, value, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_GetUsageValueArray( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection,
                                         USAGE usage, char *value_buf, USHORT value_len,
                                         PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usage %u, value_buf %p, value_len %u, "
           "preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usage, value_buf, value_len, preparsed_data, report_buf, report_len );
    return rs_hidp_get_usage_value_array( report_type, usage_page, collection, usage, value_buf, value_len,
                                          preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_SetUsageValue( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection, USAGE usage,
                                    ULONG value, PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usage %u, value %lu, preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usage, value, preparsed_data, report_buf, report_len );
    return rs_hidp_set_usage_value( report_type, usage_page, collection, usage, value, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_SetScaledUsageValue( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection,
                                          USAGE usage, LONG value, PHIDP_PREPARSED_DATA preparsed_data,
                                          char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usage %u, value %ld, preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usage, value, preparsed_data, report_buf, report_len );
    return rs_hidp_set_scaled_usage_value( report_type, usage_page, collection, usage, value, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_SetUsageValueArray( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection,
                                         USAGE usage, char *value_buf, USHORT value_len,
                                         PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usage %u, value_buf %p, value_len %u, "
           "preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usage, value_buf, value_len, preparsed_data, report_buf, report_len );
    return rs_hidp_set_usage_value_array( report_type, usage_page, collection, usage, value_buf, value_len,
                                          preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_GetUsages( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection, USAGE *usages,
                                ULONG *usages_len, PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usages %p, usages_len %p, preparsed_data %p, "
           "report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usages, usages_len, preparsed_data, report_buf, report_len );
    return rs_hidp_get_usages( report_type, usage_page, collection, usages, usages_len, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_GetUsagesEx( HIDP_REPORT_TYPE report_type, USHORT collection, USAGE_AND_PAGE *usages,
                                  ULONG *usages_len, PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, collection %u, usages %p, usages_len %p, preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, collection, usages, usages_len, preparsed_data, report_buf, report_len );
    return rs_hidp_get_usages_ex( report_type, collection, usages, usages_len, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_SetUsages( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection, USAGE *usages,
                                ULONG *usage_count, PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usages %p, usage_count %p, preparsed_data %p, "
           "report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usages, usage_count, preparsed_data, report_buf, report_len );
    return rs_hidp_set_usages( report_type, usage_page, collection, usages, usage_count, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_UnsetUsages( HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT collection, USAGE *usages,
                                  ULONG *usage_count, PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, usage_page %u, collection %u, usages %p, usage_count %p, preparsed_data %p, "
           "report_buf %p, report_len %lu.\n",
           report_type, usage_page, collection, usages, usage_count, preparsed_data, report_buf, report_len );
    return rs_hidp_unset_usages( report_type, usage_page, collection, usages, usage_count, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_GetData( HIDP_REPORT_TYPE report_type, HIDP_DATA *data, ULONG *data_len,
                              PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, data %p, data_len %p, preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, data, data_len, preparsed_data, report_buf, report_len );
    return rs_hidp_get_data( report_type, data, data_len, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_SetData( HIDP_REPORT_TYPE report_type, HIDP_DATA *data, ULONG *data_len,
                              PHIDP_PREPARSED_DATA preparsed_data, char *report_buf, ULONG report_len )
{
    TRACE( "report_type %d, data %p, data_len %p, preparsed_data %p, report_buf %p, report_len %lu.\n",
           report_type, data, data_len, preparsed_data, report_buf, report_len );
    return rs_hidp_set_data( report_type, data, data_len, preparsed_data, report_buf, report_len );
}

NTSTATUS WINAPI HidP_TranslateUsagesToI8042ScanCodes( USAGE *ChangedUsageList, ULONG UsageListLength,
                                                      HIDP_KEYBOARD_DIRECTION KeyAction,
                                                      HIDP_KEYBOARD_MODIFIER_STATE *ModifierState,
                                                      PHIDP_INSERT_SCANCODES InsertCodesProcedure, VOID *InsertCodesContext )
{
    FIXME( "ChangedUsageList %p, UsageListLength %lu, KeyAction %u, ModifierState %p, InsertCodesProcedure %p, InsertCodesContext %p stub!\n",
           ChangedUsageList, UsageListLength, KeyAction, ModifierState, InsertCodesProcedure, InsertCodesContext );

    return STATUS_NOT_IMPLEMENTED;
}
