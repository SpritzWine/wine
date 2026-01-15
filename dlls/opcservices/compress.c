/*
 * Copyright 2018 Nikolay Sivov for CodeWeavers
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

#define COBJMACROS

#include <stdint.h>
#include <stdarg.h>
#include <stdint.h>
#include <zlib.h>

#include "windef.h"
#include "winternl.h"
#include "msopc.h"
#include "xmllite.h"

#include "opc_private.h"

#include "wine/rbtree.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msopc);

#pragma pack(push,2)
struct local_file_header
{
    uint32_t signature;
    uint16_t version;
    uint16_t flags;
    uint16_t method;
    uint32_t mtime;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t name_length;
    uint16_t extra_length;
};

struct zip32_data_descriptor
{
    uint32_t signature;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
};

struct zip32_central_directory_header
{
    uint32_t signature;
    uint16_t version;
    uint16_t min_version;
    uint16_t flags;
    uint16_t method;
    uint32_t mtime;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t name_length;
    uint16_t extra_length;
    uint16_t comment_length;
    uint16_t diskid;
    uint16_t internal_attributes;
    uint32_t external_attributes;
    uint32_t local_file_offset;
};

struct zip32_end_of_central_directory
{
    uint32_t signature;
    uint16_t diskid;
    uint16_t firstdisk;
    uint16_t records_num;
    uint16_t records_total;
    uint32_t directory_size;
    uint32_t directory_offset;
    uint16_t comment_length;
};

struct zip64_data_descriptor
{
    uint32_t signature;
    uint32_t crc32;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
};

struct zip64_extra_field
{
    uint16_t id;
    uint16_t size;
    uint64_t uncompressed_size;
    uint64_t compressed_size;
    uint64_t offset;
    uint32_t diskid;
};

struct zip64_end_of_central_directory
{
    uint32_t signature;
    uint64_t size;
    uint16_t version;
    uint16_t min_version;
    uint32_t diskid;
    uint32_t directory_diskid;
    uint64_t records_num;
    uint64_t records_total;
    uint64_t directory_size;
    uint64_t directory_offset;
};

struct zip64_end_of_central_directory_locator
{
    uint32_t signature;
    uint32_t eocd64_disk;
    uint64_t eocd64_offset;
    uint32_t disk_num;
};
#pragma pack(pop)

enum zip_signatures
{
    ZIP32_CDFH = 0x02014b50,
    LOCAL_HEADER_SIGNATURE  = 0x04034b50,
    ZIP32_EOCD = 0x06054b50,
    ZIP64_EOCD64 = 0x06064b50,
    ZIP64_EOCD64_LOCATOR = 0x07064b50,
    DATA_DESCRIPTOR_SIGNATURE = 0x08074b50,
};

enum zip_versions
{
    ZIP32_VERSION = 20,
    ZIP64_VERSION = 45,
};

static const OPC_COMPRESSION_OPTIONS deflate_opts[] =
{
    OPC_COMPRESSION_NORMAL,
    OPC_COMPRESSION_MAXIMUM,
    OPC_COMPRESSION_FAST,
    OPC_COMPRESSION_SUPERFAST
};

enum entry_flags
{
    DEFLATE_NORMAL = 0x0,
    DEFLATE_MAX = 0x2,
    DEFLATE_FAST = 0x4,
    DEFLATE_SUPERFAST = 0x6,
    DEFLATE_LEVEL_MASK = 0x6,
    USE_DATA_DESCRIPTOR = 0x8,
};

struct zip_file
{
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint64_t offset;
    uint32_t crc32;
    uint16_t name_length;
    uint16_t method;
    uint16_t flags;
    char name[1];
};

struct zip_archive
{
    struct zip_file **files;
    size_t file_count;
    size_t file_size;

    DWORD mtime;
    IStream *output;
    uint64_t position;
    HRESULT status;

    bool zip64;

    unsigned char input_buffer[0x8000];
    unsigned char output_buffer[0x8000];
};

HRESULT compress_create_archive(IStream *output, bool zip64, struct zip_archive **out)
{
    struct zip_archive *archive;
    WORD date, time;
    FILETIME ft;

    if (!(archive = malloc(sizeof(*archive))))
        return E_OUTOFMEMORY;

    archive->files = NULL;
    archive->file_size = 0;
    archive->file_count = 0;
    archive->status = S_OK;

    archive->output = output;
    IStream_AddRef(archive->output);
    archive->position = 0;

    GetSystemTimeAsFileTime(&ft);
    FileTimeToDosDateTime(&ft, &date, &time);
    archive->mtime = date << 16 | time;

    archive->zip64 = zip64;

    *out = archive;

    return S_OK;
}

static void compress_write(struct zip_archive *archive, const void *data, ULONG size)
{
    ULONG written;

    if (FAILED(archive->status))
        return;

    archive->status = IStream_Write(archive->output, data, size, &written);
    if (written != size)
        archive->status = E_FAIL;
    else
        archive->position += written;

    if (FAILED(archive->status))
        WARN("Failed to write output %p, size %lu, written %lu, hr %#lx.\n", data, size, written, archive->status);
}

HRESULT compress_finalize_archive(struct zip_archive *archive)
{
    struct zip32_end_of_central_directory dir_end;
    struct zip32_central_directory_header cdh;
    uint64_t cd_offset, cd_size = 0;
    size_t i;

    /* Directory entries */
    cd_offset = archive->position;

    if (archive->zip64)
    {
        struct zip64_end_of_central_directory_locator locator;
        struct zip64_end_of_central_directory eocd64;
        uint64_t eocd64_offset = archive->position;
        struct zip64_extra_field extra_field;

        for (i = 0; i < archive->file_count; ++i)
        {
            const struct zip_file *file = archive->files[i];

            cdh.signature = ZIP32_CDFH;
            cdh.version = ZIP64_VERSION;
            cdh.min_version = ZIP64_VERSION;
            cdh.flags = file->flags;
            cdh.method = file->method;
            cdh.mtime = archive->mtime;
            cdh.crc32 = file->crc32;
            cdh.compressed_size = ~0u;
            cdh.uncompressed_size = ~0u;
            cdh.name_length = file->name_length;
            cdh.extra_length = sizeof(extra_field);
            cdh.comment_length = 0;
            cdh.diskid = 0;
            cdh.internal_attributes = 0;
            cdh.external_attributes = 0;
            cdh.local_file_offset = ~0u;
            compress_write(archive, &cdh, sizeof(cdh));

            /* File name */
            compress_write(archive, file->name, file->name_length);

            /* Extra field */
            extra_field.id = 1;
            extra_field.size = sizeof(extra_field);
            extra_field.uncompressed_size = file->uncompressed_size;
            extra_field.compressed_size = file->compressed_size;
            extra_field.offset = file->offset;
            extra_field.diskid = 0;
            compress_write(archive, &extra_field, sizeof(extra_field));

            cd_size += cdh.name_length + cdh.extra_length + sizeof(cdh);
        }

        /* ZIP64 end of central directory */
        eocd64.signature = ZIP64_EOCD64;
        eocd64.size = sizeof(eocd64) - 12;
        eocd64.version = ZIP64_VERSION;
        eocd64.min_version = ZIP64_VERSION;
        eocd64.diskid = 0;
        eocd64.directory_diskid = 0;
        eocd64.records_num = archive->file_count;
        eocd64.records_total = archive->file_count;
        eocd64.directory_size = cd_size;
        eocd64.directory_offset = cd_offset;
        compress_write(archive, &eocd64, sizeof(eocd64));

        /* ZIP64 end of central directory locator */
        locator.signature = ZIP64_EOCD64_LOCATOR;
        locator.eocd64_disk = 0;
        locator.eocd64_offset = eocd64_offset;
        locator.disk_num = 0;
        compress_write(archive, &locator, sizeof(locator));

        /* End of central directory */
        memset(&dir_end, 0xff, sizeof(dir_end));
        dir_end.signature = ZIP32_EOCD;
        dir_end.comment_length = 0;
        compress_write(archive, &dir_end, sizeof(dir_end));
    }
    else
    {
        for (i = 0; i < archive->file_count; ++i)
        {
            const struct zip_file *file = archive->files[i];

            cdh.signature = ZIP32_CDFH;
            cdh.version = ZIP32_VERSION;
            cdh.min_version = ZIP32_VERSION;
            cdh.flags = file->flags;
            cdh.method = file->method;
            cdh.mtime = archive->mtime;
            cdh.crc32 = file->crc32;
            cdh.compressed_size = file->compressed_size;
            cdh.uncompressed_size = file->uncompressed_size;
            cdh.name_length = file->name_length;
            cdh.extra_length = 0;
            cdh.comment_length = 0;
            cdh.diskid = 0;
            cdh.internal_attributes = 0;
            cdh.external_attributes = 0;
            cdh.local_file_offset = file->offset;
            compress_write(archive, &cdh, sizeof(cdh));

            /* File name */
            compress_write(archive, file->name, file->name_length);

            cd_size += cdh.name_length + sizeof(cdh);
        }

        /* End of central directory */
        dir_end.signature = ZIP32_EOCD;
        dir_end.diskid = 0;
        dir_end.firstdisk = 0;
        dir_end.records_num = archive->file_count;
        dir_end.records_total = archive->file_count;
        dir_end.directory_size = cd_size;
        dir_end.directory_offset = cd_offset;
        dir_end.comment_length = 0;
        compress_write(archive, &dir_end, sizeof(dir_end));
    }

    return archive->status;
}

void compress_release_archive(struct zip_archive *archive)
{
    size_t i;

    IStream_Release(archive->output);

    for (i = 0; i < archive->file_count; i++)
        free(archive->files[i]);
    free(archive->files);
    free(archive);
}

static void *zalloc(void *opaque, unsigned int items, unsigned int size)
{
    return malloc(items * size);
}

static void zfree(void *opaque, void *ptr)
{
    free(ptr);
}

static void compress_write_content(struct zip_archive *archive, IStream *content,
        OPC_COMPRESSION_OPTIONS options, struct zip_file *file)
{
    int level, flush;
    z_stream z_str;
    LARGE_INTEGER move;
    ULONG num_read;
    HRESULT hr;
    int init_ret;

    if (FAILED(archive->status))
        return;

    file->crc32 = RtlComputeCrc32(0, NULL, 0);
    file->compressed_size = file->uncompressed_size = 0;

    move.QuadPart = 0;
    IStream_Seek(content, move, STREAM_SEEK_SET, NULL);

    switch (options)
    {
    case OPC_COMPRESSION_NONE:
        level = Z_NO_COMPRESSION;
        break;
    case OPC_COMPRESSION_NORMAL:
        level = Z_DEFAULT_COMPRESSION;
        break;
    case OPC_COMPRESSION_MAXIMUM:
        level = Z_BEST_COMPRESSION;
        break;
    case OPC_COMPRESSION_FAST:
        level = 2;
        break;
    case OPC_COMPRESSION_SUPERFAST:
        level = Z_BEST_SPEED;
        break;
    default:
        WARN("Unsupported compression options %d.\n", options);
        level = Z_DEFAULT_COMPRESSION;
    }

    memset(&z_str, 0, sizeof(z_str));
    z_str.zalloc = zalloc;
    z_str.zfree = zfree;
    if ((init_ret = deflateInit2(&z_str, level, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY)) != Z_OK)
    {
        WARN("deflateInit2() failed, ret %d.\n", init_ret);
        archive->status = OPC_E_ZIP_COMPRESSION_FAILED;
        return;
    }

    do
    {
        int ret;

        if (FAILED(hr = IStream_Read(content, archive->input_buffer, sizeof(archive->input_buffer), &num_read)))
        {
            archive->status = hr;
            break;
        }

        z_str.avail_in = num_read;
        z_str.next_in = archive->input_buffer;
        file->crc32 = RtlComputeCrc32(file->crc32, archive->input_buffer, num_read);
        file->uncompressed_size += num_read;

        flush = sizeof(archive->input_buffer) > num_read ? Z_FINISH : Z_NO_FLUSH;

        do
        {
            ULONG have;

            z_str.avail_out = sizeof(archive->output_buffer);
            z_str.next_out = archive->output_buffer;

            if ((ret = deflate(&z_str, flush)) < 0)
            {
                WARN("Failed to deflate(), ret %d.\n", ret);
                archive->status = OPC_E_ZIP_COMPRESSION_FAILED;
                break;
            }
            have = sizeof(archive->output_buffer) - z_str.avail_out;
            compress_write(archive, archive->output_buffer, have);

            file->compressed_size += have;
        } while (z_str.avail_out == 0);
    } while (flush != Z_FINISH);

    deflateEnd(&z_str);
}

HRESULT compress_add_file(struct zip_archive *archive, const WCHAR *path,
        IStream *content, OPC_COMPRESSION_OPTIONS options)
{
    struct local_file_header local_header;
    struct zip_file *file;
    DWORD len;

    len = WideCharToMultiByte(CP_ACP, 0, path, -1, NULL, 0, NULL, NULL);

    if (!(file = calloc(1, offsetof(struct zip_file, name[len]))))
        return E_OUTOFMEMORY;

    WideCharToMultiByte(CP_ACP, 0, path, -1, file->name, len, NULL, NULL);
    file->offset = archive->position;
    file->name_length = len - 1;
    if (options != OPC_COMPRESSION_NONE)
    {
        file->method = Z_DEFLATED;
        if (options == OPC_COMPRESSION_MAXIMUM)
            file->flags = DEFLATE_MAX;
        else if (options == OPC_COMPRESSION_FAST)
            file->flags = DEFLATE_FAST;
        else if (options == OPC_COMPRESSION_SUPERFAST)
            file->flags = DEFLATE_SUPERFAST;
        else
            file->flags = DEFLATE_NORMAL;
    }
    file->flags |= USE_DATA_DESCRIPTOR;

    local_header.signature = LOCAL_HEADER_SIGNATURE;
    local_header.flags = USE_DATA_DESCRIPTOR;
    local_header.method = file->method;
    local_header.mtime = archive->mtime;
    local_header.crc32 = 0;
    local_header.name_length = len - 1;
    local_header.extra_length = 0;
    if (archive->zip64)
    {
        local_header.version = ZIP64_VERSION;
        local_header.compressed_size = ~0u;
        local_header.uncompressed_size = ~0u;
    }
    else
    {
        local_header.version = ZIP32_VERSION;
        local_header.compressed_size = 0;
        local_header.uncompressed_size = 0;
    }

    compress_write(archive, &local_header, sizeof(local_header));
    compress_write(archive, file->name, file->name_length);

    /* Content */
    compress_write_content(archive, content, options, file);

    /* Data descriptor */
    if (archive->zip64)
    {
        struct zip64_data_descriptor zip64_data_desc;

        zip64_data_desc.signature = DATA_DESCRIPTOR_SIGNATURE;
        zip64_data_desc.crc32 = file->crc32;
        zip64_data_desc.compressed_size = file->compressed_size;
        zip64_data_desc.uncompressed_size = file->uncompressed_size;
        compress_write(archive, &zip64_data_desc, sizeof(zip64_data_desc));
    }
    else
    {
        struct zip32_data_descriptor zip32_data_desc;

        zip32_data_desc.signature = DATA_DESCRIPTOR_SIGNATURE;
        zip32_data_desc.crc32 = file->crc32;
        zip32_data_desc.compressed_size = file->compressed_size;
        zip32_data_desc.uncompressed_size = file->uncompressed_size;
        compress_write(archive, &zip32_data_desc, sizeof(zip32_data_desc));
    }

    if (FAILED(archive->status))
    {
        free(file);
        return archive->status;
    }

    if (!opc_array_reserve((void **)&archive->files, &archive->file_size, archive->file_count + 1,
            sizeof(*archive->files)))
    {
        free(file);
        return E_OUTOFMEMORY;
    }

    archive->files[archive->file_count++] = file;

    return S_OK;
}

static const char *debugstr_OPC_COMPRESSION_OPTIONS(OPC_COMPRESSION_OPTIONS opt)
{
    static const char *str[] = {"OPC_COMPRESSION_NONE", "OPC_COMPRESSION_NORMAL", "OPC_COMPRESSION_MAXIMUM",
                                "OPC_COMPRESSION_FAST", "OPC_COMPRESSION_SUPERFAST"};

    if (opt + 1 < ARRAY_SIZE(str))
        return str[opt + 1];
    return wine_dbg_sprintf("%#x", opt);
}

static const char *debugstr_zip_entry(const struct zip_entry *entry)
{
    return wine_dbg_sprintf("{%I64u %I64u %#lx %#I64x}", entry->compressed_size, entry->uncompressed_size, entry->crc32,
                            entry->local_file_offset.QuadPart);
}
struct zip_part
{
    struct zip_entry entry;
    OPC_COMPRESSION_OPTIONS opt;
    IOpcPartUri *name;
};

static BOOL compress_validate_part_size(const struct zip_entry *entry, const struct local_file_header *file,
                                        const struct zip64_extra_field *ext)
{
    if (file->flags & USE_DATA_DESCRIPTOR)
        return !(file->compressed_size || file->uncompressed_size || file->crc32 || ext->compressed_size ||
                 ext->uncompressed_size);
    else
    {
        ULONG64 compressed_size = file->compressed_size == UINT32_MAX ? ext->compressed_size : file->compressed_size;
        ULONG64 uncompressed_size = file->uncompressed_size == UINT32_MAX ? ext->uncompressed_size : file->uncompressed_size;

        return compressed_size == entry->compressed_size && uncompressed_size == entry->uncompressed_size &&
               file->crc32 == entry->crc32;
    }
}

/* Decompress the ZIP file described by entry in IStream archive into out. */
HRESULT decompress_to_stream(IStream *archive, const struct zip_entry *entry, IStream *out, BOOL set_size)
{
    ULONG64 compressed_data_read = 0, data_uncompressed = 0;
    ULONG ext_size = 0, read, crc32 = 0, exp_crc32 = 0;
    static const ULONG buffer_size = 0x8000;
    struct zip64_extra_field ext = {0};
    struct local_file_header file = {0};
    BYTE *input_buf, *output_buf;
    z_stream z_str = {0};
    LARGE_INTEGER off;
    HRESULT hr;
    int ret;

    off.QuadPart = entry->local_file_offset.QuadPart;
    if (FAILED(hr = IStream_Seek(archive, off, STREAM_SEEK_SET, NULL)))
        return hr;
    hr = IStream_Read(archive, &file, sizeof(file), &read);
    if (hr != S_OK)
        return hr == S_FALSE ? OPC_E_ZIP_DECOMPRESSION_FAILED : hr;
    if (file.method && file.method != Z_DEFLATED)
        return OPC_E_ZIP_UNSUPPORTEDARCHIVE;
    if (file.uncompressed_size == UINT32_MAX)
        ext_size = 8;
    if (file.compressed_size == UINT32_MAX)
        ext_size = 16;
    if (ext_size)
        ext_size += 4; /* For the signature and size fields. */
    if (file.extra_length < ext_size)
        return OPC_E_ZIP_CORRUPTED_ARCHIVE;

    off.QuadPart = file.name_length;
    if (FAILED(hr = IStream_Seek(archive, off, STREAM_SEEK_CUR, NULL)))
        return hr;
    if (ext_size && ((hr = IStream_Read(archive, &ext, ext_size, &read) != S_OK)))
        return hr == S_FALSE ? OPC_E_ZIP_DECOMPRESSION_FAILED : hr;
    if (!compress_validate_part_size(entry, &file, &ext))
        return OPC_E_ZIP_CORRUPTED_ARCHIVE;
    if (set_size && FAILED(hr = IStream_SetSize(out, *(ULARGE_INTEGER *)&entry->uncompressed_size)))
        return hr;
    if (!(file.flags & USE_DATA_DESCRIPTOR))
        exp_crc32 = file.crc32;
    if (!(input_buf = malloc(buffer_size))) return E_OUTOFMEMORY;
    if (!(output_buf = malloc(buffer_size)))
    {
        free(input_buf);
        return E_OUTOFMEMORY;
    }

    z_str.zalloc = zalloc;
    z_str.zfree = zfree;
    z_str.next_in = input_buf;
    z_str.next_out = output_buf;
    z_str.avail_out = buffer_size;
    ret = inflateInit2(&z_str, -MAX_WBITS);
    if (ret)
    {
        free(input_buf);
        free(output_buf);
        return OPC_E_ZIP_DECOMPRESSION_FAILED;
    }
    hr = OPC_E_ZIP_DECOMPRESSION_FAILED;
    crc32 = RtlComputeCrc32(0, NULL, 0);
    while (compressed_data_read < entry->compressed_size && data_uncompressed < entry->uncompressed_size)
    {
        ULONG to_read = min(entry->compressed_size - compressed_data_read, buffer_size);

        z_str.total_out = 0;
        hr = IStream_Read(archive, input_buf, to_read, &read);
        if (hr != S_OK)
        {
            if (hr == S_FALSE) hr = OPC_E_ZIP_DECOMPRESSION_FAILED;
            break;
        }
        z_str.avail_in = read;
        ret = inflate(&z_str, Z_SYNC_FLUSH);
        if (ret && ret != Z_STREAM_END)
        {
            hr = OPC_E_ZIP_DECOMPRESSION_FAILED;
            break;
        }
        if (FAILED(hr = IStream_Write(out, output_buf, z_str.total_out, NULL)))
            break;
        crc32 = RtlComputeCrc32(crc32, output_buf, z_str.total_out);
        compressed_data_read += read;
        data_uncompressed += z_str.total_out;
    }
    inflateEnd(&z_str);
    free(input_buf);
    free(output_buf);
    if (FAILED(hr))
        return hr;
    if (compressed_data_read != entry->compressed_size || data_uncompressed != entry->uncompressed_size)
        return OPC_E_ZIP_DECOMPRESSION_FAILED;

    if (file.flags & USE_DATA_DESCRIPTOR)
    {
        ULONG64 exp_compressed, exp_uncompressed;
        union {
            struct zip32_data_descriptor desc32;
            struct zip64_data_descriptor desc64;
        } desc = {0};

        hr = IStream_Read(archive, &desc, ext_size ? sizeof(desc.desc64) : sizeof(desc.desc32), &read);
        if (hr != S_OK) return hr == S_FALSE ? OPC_E_ZIP_DECOMPRESSION_FAILED : hr;
        if (ext_size)
        {
            exp_compressed = desc.desc64.compressed_size;
            exp_uncompressed = desc.desc64.uncompressed_size;
        }
        else
        {
            exp_compressed = desc.desc32.compressed_size;
            exp_uncompressed = desc.desc32.uncompressed_size;
        }
        if (exp_compressed != compressed_data_read || exp_uncompressed != data_uncompressed)
            return OPC_E_ZIP_CORRUPTED_ARCHIVE;
        exp_crc32 = desc.desc64.crc32;
    }
    return exp_crc32 != crc32 ? OPC_E_ZIP_CORRUPTED_ARCHIVE : S_OK;
}

struct content_type_entry
{
    struct rb_entry entry;
    WCHAR *extension_or_part_name;
    WCHAR *type;
};

static int content_type_entry_compare(const void *key, const struct rb_entry *entry)
{
    const WCHAR *key2 = RB_ENTRY_VALUE(entry, struct content_type_entry, entry)->extension_or_part_name;
    return wcsicmp(key, key2);
}

static void content_type_entry_destroy(struct rb_entry *entry, void *data)
{
    struct content_type_entry *ct = RB_ENTRY_VALUE(entry, struct content_type_entry, entry);

    free(ct->extension_or_part_name);
    free(ct->type);
    free(ct);
}

static HRESULT xml_get_attribute(IXmlReader *reader, const WCHAR *name, WCHAR **val_ret)
{
    const WCHAR *val;
    HRESULT hr;

    hr = IXmlReader_MoveToAttributeByName(reader, name, NULL);
    if (hr != S_OK)
        return FAILED(hr) ? hr : OPC_E_INVALID_CONTENT_TYPE_XML;
    if (SUCCEEDED(hr = IXmlReader_GetValue(reader, &val, NULL)))
        hr = (*val_ret = wcsdup(val)) ? S_OK : E_OUTOFMEMORY;
    return hr;
}

static HRESULT xml_get_next_node(IXmlReader *reader, XmlNodeType exp_type, const WCHAR *exp_name)
{
    XmlNodeType type;
    HRESULT hr;

    do {
        hr = IXmlReader_Read(reader, &type);
        if (hr != S_OK)
            return hr;
    } while (type == XmlNodeType_Whitespace);
    if (exp_type != XmlNodeType_None && type != exp_type)
        return OPC_E_INVALID_CONTENT_TYPE_XML;
    if (exp_name)
    {
        const WCHAR *name;

        if (SUCCEEDED(hr = IXmlReader_GetLocalName(reader, &name, NULL)) && wcscmp(name, exp_name))
            hr = OPC_E_INVALID_CONTENT_TYPE_XML;
    }
    return hr;
}

static HRESULT content_types_add_type(IXmlReader *reader, const WCHAR *attr_name, struct rb_tree *types)
{
    WCHAR *ext_or_part = NULL, *content_type = NULL;
    struct content_type_entry *type = NULL;
    HRESULT hr;

    if (FAILED(hr = xml_get_attribute(reader, attr_name, &ext_or_part))) return hr;
    if (FAILED(hr = xml_get_attribute(reader, L"ContentType", &content_type)))
        goto done;
    if (!(type = calloc(1, sizeof(*type))))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    type->extension_or_part_name = ext_or_part;
    type->type = content_type;
    if (rb_put(types, ext_or_part, &type->entry))
        hr = OPC_E_DUPLICATE_DEFAULT_EXTENSION;
done:
    if (FAILED(hr))
    {
        free(content_type);
        free(ext_or_part);
        free(type);
    }
    return hr;
}

static HRESULT content_types_add_default(IXmlReader *reader, struct rb_tree *defaults)
{
    return content_types_add_type(reader, L"Extension", defaults);
}

static HRESULT content_types_add_override(IXmlReader *reader, struct rb_tree *overrides)
{
    return content_types_add_type(reader, L"PartName", overrides);
}

static HRESULT compress_read_content_types(IStream *archive, const struct zip_entry *content_types,
                                           struct rb_tree *defaults, struct rb_tree *overrides)
{
    static const LARGE_INTEGER start = {0};
    IXmlReader *reader;
    IStream *xml;
    HRESULT hr;

    if (FAILED(hr = CreateStreamOnHGlobal(NULL, TRUE, &xml))) return hr;
    if (FAILED(hr = decompress_to_stream(archive, content_types, xml, TRUE)))
    {
        IStream_Release(xml);
        return hr;
    }
    if (FAILED(hr = IStream_Seek(xml, start, STREAM_SEEK_SET, NULL)))
    {
        IStream_Release(xml);
        return hr;
    }
    if (FAILED(hr = CreateXmlReader(&IID_IXmlReader, (void *)&reader, NULL)))
    {
        IStream_Release(xml);
        return hr;
    }

    hr = IXmlReader_SetInput(reader, (IUnknown *)xml);
    IStream_Release(xml);
    if (FAILED(hr)) goto done;

    if (FAILED(hr = xml_get_next_node(reader, XmlNodeType_XmlDeclaration, L"xml")))
        goto done;
    if (FAILED(hr = xml_get_next_node(reader, XmlNodeType_Element, L"Types")))
        goto done;
    while(SUCCEEDED(hr = xml_get_next_node(reader, XmlNodeType_None, NULL)))
    {
        const WCHAR *name;
        XmlNodeType type;

        if (FAILED(hr = IXmlReader_GetNodeType(reader, &type))) break;
        if (type != XmlNodeType_Element && type != XmlNodeType_EndElement) continue;
        if (FAILED(hr = IXmlReader_GetLocalName(reader, &name, NULL))) break;
        if (type == XmlNodeType_EndElement) break;

        if (!wcscmp(name, L"Default"))
            hr = content_types_add_default(reader, defaults);
        else if (!wcscmp(name, L"Override"))
            hr = content_types_add_override(reader, overrides);
        else
            FIXME("Unknown node: %s\n", debugstr_w(name));

        if (FAILED(hr)) break;
    }
done:
    IXmlReader_Release(reader);
    return hr;
}

/* The stream should be at the start of the central directory. */
static HRESULT compress_read_entries(IOpcFactory *factory, IStream *stream, OPC_READ_FLAGS flags, ULONG dir_records,
                                     struct opc_part_set *partset)
{
    static const char *content_types_name = "[Content_Types].xml";
    ULONG i, part_count = 0, nameW_len = 0, nameA_len = 0;
    struct zip_entry content_types = {0};
    BOOL found_content_types = FALSE;
    struct zip_part *parts = NULL;
    struct rb_tree type_overrides;
    struct rb_tree type_defaults;
    WCHAR *nameW = NULL;
    char *nameA = NULL;
    HRESULT hr;

    rb_init(&type_defaults, content_type_entry_compare);
    rb_init(&type_overrides, content_type_entry_compare);

    for (i = 0; i < dir_records; i++)
    {
        struct zip32_central_directory_header dir = {0};
        struct zip64_extra_field ext = {0};
        struct zip_entry entry = {0};
        OPC_COMPRESSION_OPTIONS opt;
        ULONG read, ext_size = 0;
        LARGE_INTEGER off;

        hr = IStream_Read(stream, &dir, sizeof(dir), &read);
        if (hr != S_OK)
            goto done;
        if (dir.name_length + 1 > nameA_len)
        {
            char *tmp;

            nameA_len = dir.name_length + 1;
            if (!(tmp = realloc(nameA, nameA_len)))
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }
            nameA = tmp;
        }
        nameA[dir.name_length] = '\0';

        hr = IStream_Read(stream, nameA, dir.name_length, &read);
        if (hr != S_OK)
            goto done;
        /* If any of the fields are UINT32_MAX, there is an extra ZIP64 extended information field */
        if (dir.uncompressed_size == UINT32_MAX)
            ext_size = 8;
        if (dir.compressed_size == UINT32_MAX)
            ext_size = 16;
        if (dir.local_file_offset == UINT32_MAX)
            ext_size = 24;
        if (ext_size)
            ext_size += 4; /* For the signature and size fields. */
        /* Make sure that the central directory record actually indicates there is a ZIP64 extended field. */
        if (dir.extra_length < ext_size)
        {
            hr = OPC_E_ZIP_CORRUPTED_ARCHIVE;
            goto done;
        }
        if (ext_size && ((hr = IStream_Read(stream, &ext, ext_size, &read) != S_OK)))
            goto done;

        off.QuadPart = dir.extra_length - ext_size;
        /* Skip any extra data that we haven't read. */
        if (off.QuadPart && FAILED(hr = IStream_Seek(stream, off, STREAM_SEEK_CUR, NULL)))
            goto done;
        opt = dir.method ? deflate_opts[(dir.flags & DEFLATE_LEVEL_MASK) >> 1] : OPC_COMPRESSION_NONE;
        entry.uncompressed_size = (dir.uncompressed_size == UINT32_MAX) ? ext.uncompressed_size : dir.uncompressed_size;
        entry.compressed_size = (dir.compressed_size == UINT32_MAX) ? ext.compressed_size : dir.compressed_size;
        entry.crc32 = dir.crc32;
        entry.local_file_offset.QuadPart = (dir.local_file_offset == UINT32_MAX) ? ext.offset : dir.local_file_offset;

        off.QuadPart = dir.comment_length;
        if (dir.comment_length && FAILED(hr = IStream_Seek(stream, off, STREAM_SEEK_CUR, NULL)))
            goto done;
        if (!strcmp(nameA, content_types_name))
        {
            if (found_content_types)
            {
                hr = OPC_E_ZIP_DUPLICATE_NAME;
                goto done;
            }
            content_types = entry;
            found_content_types = TRUE;
        }
        else
        {
            IOpcPartUri *name_uri;
            struct zip_part *tmp;
            INT len;

            TRACE("Adding new OPC part %s: %s, %s\n", debugstr_a(nameA), debugstr_OPC_COMPRESSION_OPTIONS(opt),
                  debugstr_zip_entry(&entry));

            len = MultiByteToWideChar(CP_ACP, 0, nameA, -1, NULL, 0);
            if (len > nameW_len)
            {
                WCHAR *tmp;

                nameW_len = len;
                if (!(tmp = realloc(nameW, len * sizeof(WCHAR))))
                {
                    hr = E_OUTOFMEMORY;
                    goto done;
                }
                nameW = tmp;
            }
            MultiByteToWideChar(CP_ACP, 0, nameA, -1, nameW, len);
            if (FAILED(hr = IOpcFactory_CreatePartUri(factory, nameW, &name_uri)))
                goto done;
            if (!(tmp = realloc(parts, (part_count + 1) * sizeof(*parts))))
            {
                IOpcPartUri_Release(name_uri);
                hr = E_OUTOFMEMORY;
                goto done;
            }
            parts = tmp;
            tmp = &parts[part_count++];
            tmp->entry = entry;
            tmp->opt = opt;
            tmp->name = name_uri;
        }
    }

    if (!found_content_types)
    {
        hr = OPC_E_MISSING_CONTENT_TYPES;
        goto done;
    }
    hr = compress_read_content_types(stream, &content_types, &type_defaults, &type_overrides);

    for (i = 0; i < part_count && SUCCEEDED(hr); i++)
    {
        const struct content_type_entry *content_type;
        const struct rb_entry *entry;
        BSTR str;

        if (FAILED(hr = IOpcPartUri_GetAbsoluteUri(parts[i].name, &str))) goto done;
        /* First, check if if there is "Override" entry for this part. */
        entry = rb_get(&type_overrides, str);
        SysFreeString(str);
        if (!entry)
        {
            if (FAILED(hr = IOpcPartUri_GetExtension(parts[i].name, &str))) goto done;
            entry = rb_get(&type_defaults, &str[1]); /* Omit the leading dot. */
        }
        if (!entry)
        {
            hr = E_INVALIDARG;
            goto done;
        }

        content_type = RB_ENTRY_VALUE(entry, struct content_type_entry, entry);
        hr = opc_part_set_add_zip_part(partset, stream, &parts[i].entry, parts[i].opt, flags, parts[i].name,
                                       content_type->type);
        if (FAILED(hr)) goto done;
    }

done:
    rb_destroy(&type_defaults, content_type_entry_destroy, NULL);
    rb_destroy(&type_overrides, content_type_entry_destroy, NULL);
    for (i = 0; i < part_count; i++)
        IOpcPartUri_Release(parts[i].name);
    free(parts);
    free(nameA);
    free(nameW);
    return hr == S_FALSE ? OPC_E_ZIP_CORRUPTED_ARCHIVE : hr;
}

HRESULT compress_open_archive(IOpcFactory *factory, IStream *stream, OPC_READ_FLAGS flags,
                              struct opc_part_set *part_set)
{
    struct zip32_end_of_central_directory end = {0};
    ULARGE_INTEGER end_dir_start;
    LARGE_INTEGER off;
    HRESULT hr;

    off.QuadPart = -sizeof(end);
    hr = IStream_Seek(stream, off, STREAM_SEEK_END, &end_dir_start);
    /* Start from the end of the archive to find a end of central directory header. */
    while (SUCCEEDED(hr))
    {
        struct zip64_end_of_central_directory_locator locator = {0};
        struct zip64_end_of_central_directory end64 = {0};
        LARGE_INTEGER central_dir_start;
        ULONG read, dir_records;

        if (FAILED(hr = IStream_Read(stream, &end, sizeof(end), &read)))
            return hr;
        if (hr == S_FALSE || end.signature != ZIP32_EOCD)
            goto next;
        /* Check any of the fields in the header are UINT16_MAX, and there are enough bytes for a EOCD locator and EOCD64 record.
         * In that case, this might be a ZIP64 archive. */
        if ((end.records_total == UINT16_MAX || end.directory_size == UINT16_MAX || end.directory_offset == UINT16_MAX) &&
            end_dir_start.QuadPart >= (sizeof(locator) + sizeof(end64)))
        {
                ULARGE_INTEGER loc_start, end64_start;

                /* ZIP64 uses an additional End of Central Directory Locator record. */
                off.QuadPart = end_dir_start.QuadPart - sizeof(locator);
                if (FAILED(hr = IStream_Seek(stream, off, STREAM_SEEK_SET, &loc_start)))
                    goto next;
                if (FAILED(hr = IStream_Read(stream, &locator, sizeof(locator), &read)))
                    return hr;
                if (hr == S_FALSE || locator.signature != ZIP64_EOCD64_LOCATOR ||
                    locator.eocd64_offset >= loc_start.QuadPart ||
                    loc_start.QuadPart - locator.eocd64_offset < sizeof(end64))
                    goto next;
                off.QuadPart = locator.eocd64_offset;
                if (FAILED(hr = IStream_Seek(stream, off, STREAM_SEEK_SET, &end64_start)))
                    goto next;
                if (FAILED(hr = IStream_Read(stream, &end64, sizeof(end64), &read)))
                    return hr;
                if (hr == S_FALSE || end64.signature != ZIP64_EOCD64 || end64.size < (sizeof(end64) - 12))
                    goto next;
                central_dir_start.QuadPart = end64.directory_offset;
                dir_records = end64.records_total;
        }
        else /* Otherwise, treat it as ZIP32. */
        {
            /* Check if end.directory_offset actually points to the start of central directory. */
            if (end.directory_offset >= end_dir_start.QuadPart ||
                end_dir_start.QuadPart - end.directory_offset < sizeof(end))
                goto next;
            central_dir_start.QuadPart = end.directory_offset;
            dir_records = end.records_total;
        }

        /* The central directory can't be located after the EOCD header. */
        if (central_dir_start.QuadPart >= end_dir_start.QuadPart)
            goto next;
        if (FAILED(hr = IStream_Seek(stream, central_dir_start, STREAM_SEEK_SET, NULL)))
            goto next;
        return compress_read_entries(factory, stream, flags, dir_records, part_set);

    /* Seek back sizeof(end) bytes from where we initially were, and try again. */
    next:
        if (FAILED(hr = IStream_Seek(stream, *(LARGE_INTEGER *)&end_dir_start, STREAM_SEEK_SET, NULL)))
            return hr;
        off.QuadPart = -sizeof(end);
        hr = IStream_Seek(stream, off, STREAM_SEEK_CUR, &end_dir_start);
    }

    return hr;
}
