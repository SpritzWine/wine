/*
 * Copyright 2025 Rémi Bernon for CodeWeavers
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

#include <stddef.h>
#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"

#include "gst_private.h"

#include "wine/winedmo.h"
#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

struct demuxer
{
    struct winedmo_stream *stream;
    UINT64 stream_size;
    wg_parser_t wg_parser;
    HANDLE thread;
    LONG shutdown;
};

static struct demuxer *demuxer_from_handle(struct winedmo_demuxer handle)
{
    return (struct demuxer *)(UINT_PTR)handle.handle;
}

static DWORD CALLBACK read_thread(void *arg)
{
    struct demuxer *demuxer = arg;
    QWORD file_size = demuxer->stream_size;
    size_t buffer_size = 4096;
    void *data;

    if (!(data = malloc(buffer_size)))
        return 0;

    TRACE("Starting read thread for demuxer %p.\n", demuxer);

    while (!ReadAcquire(&demuxer->shutdown))
    {
        uint64_t offset;
        ULONG ret_size;
        uint32_t size;
        HRESULT hr;

        if (!wg_parser_get_next_read_offset(demuxer->wg_parser, &offset, &size))
            continue;

        if (offset >= file_size)
            size = 0;
        else if (offset + size >= file_size)
            size = file_size - offset;

        /* Some IMFByteStreams (including the standard file-based stream) return
         * an error when reading past the file size. */
        if (!size)
        {
            wg_parser_push_data(demuxer->wg_parser, data, 0);
            continue;
        }

        if (!array_reserve(&data, &buffer_size, size, 1))
        {
            free(data);
            return 0;
        }

        ret_size = size;
        if (FAILED(hr = HRESULT_FROM_NT(demuxer->stream->p_seek(demuxer->stream, &offset)))
                || FAILED(hr = HRESULT_FROM_NT(demuxer->stream->p_read(demuxer->stream, data, &ret_size))))
            ERR("Failed to read %u bytes at offset %I64u, hr %#lx.\n", size, offset, hr);
        if (ret_size != size)
            ERR("Unexpected short read: requested %u bytes, got %lu.\n", size, ret_size);
        wg_parser_push_data(demuxer->wg_parser, SUCCEEDED(hr) ? data : NULL, ret_size);
    }

    free(data);
    TRACE("Media source is shutting down; exiting.\n");
    return 0;
}

NTSTATUS CDECL winedmo_demuxer_check(const char *mime_type)
{
    return STATUS_SUCCESS;
}

NTSTATUS CDECL winedmo_demuxer_create(const WCHAR *url, struct winedmo_stream *stream, UINT64 stream_size,
        INT64 *duration, UINT *stream_count, WCHAR *mime_type, struct winedmo_demuxer *demuxer)
{
    struct demuxer *object;

    TRACE("url %s, stream %p, stream_size %#I64x, mime_type %p, demuxer %p\n", debugstr_w(url),
            stream, stream_size, mime_type, demuxer);

    if (!init_gstreamer())
        return STATUS_UNSUCCESSFUL;

    if (!(object = calloc(1, sizeof(*object))))
        return STATUS_NO_MEMORY;
    object->stream = stream;
    object->stream_size = stream_size;

    if (!(object->wg_parser = wg_parser_create(TRUE)))
        goto failed;
    if (!(object->thread = CreateThread(NULL, 0, read_thread, object, 0, NULL)))
        goto failed;
    if (FAILED(wg_parser_connect(object->wg_parser, stream_size, url)))
        goto failed;

    *duration = 0;
    *stream_count = wg_parser_get_stream_count(object->wg_parser);
    wcscpy(mime_type, L"video/x-application");

    for (UINT i = 0; i < *stream_count; i++)
    {
        wg_parser_stream_t wg_stream = wg_parser_get_stream(object->wg_parser, i);
        *duration = max(*duration, wg_parser_stream_get_duration(wg_stream));
    }

    demuxer->handle = (UINT_PTR)object;
    TRACE("created demuxer %#I64x, stream %p, duration %I64d, stream_count %u, mime_type %s\n",
            demuxer->handle, stream, *duration, *stream_count, debugstr_w(mime_type));
    return STATUS_SUCCESS;

failed:
    if (object->thread)
    {
        WriteRelease(&object->shutdown, 1);
        WaitForSingleObject(object->thread, INFINITE);
        CloseHandle(object->thread);
    }

    wg_parser_destroy(object->wg_parser);
    free(object);
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS CDECL winedmo_demuxer_destroy(struct winedmo_demuxer *demuxer)
{
    struct demuxer *object = demuxer_from_handle(*demuxer);

    if (!object)
        return STATUS_SUCCESS;

    TRACE("demuxer %#I64x\n", demuxer->handle);

    wg_parser_disconnect(object->wg_parser);

    WriteRelease(&object->shutdown, 1);
    WaitForSingleObject(object->thread, INFINITE);
    CloseHandle(object->thread);

    wg_parser_destroy(object->wg_parser);
    free(object);

    demuxer->handle = 0;
    return STATUS_SUCCESS;
}

static void buffer_lock( DMO_OUTPUT_DATA_BUFFER *buffer, BYTE **data, DWORD *size )
{
    HRESULT hr;

    if (FAILED(hr = IMediaBuffer_GetBufferAndLength( buffer->pBuffer, data, size )))
        ERR( "Failed to get media buffer data %p, hr %#lx\n", buffer, hr );
    if (FAILED(hr = IMediaBuffer_GetMaxLength( buffer->pBuffer, size )))
        ERR( "Failed to get media buffer max length %p, hr %#lx\n", buffer, hr );
}

static void buffer_unlock( DMO_OUTPUT_DATA_BUFFER *buffer, struct wg_parser_buffer *sample )
{
    IMFSample *object;
    HRESULT hr;

    if (FAILED(hr = IMediaBuffer_SetLength( buffer->pBuffer, sample->size )))
        ERR( "Failed to update buffer length, hr %#lx\n", hr );

    buffer->dwStatus = 0;
    if (SUCCEEDED(hr = IMediaBuffer_QueryInterface( buffer->pBuffer, &IID_IMFSample, (void **)&object )))
    {
        if (sample->has_pts) IMFSample_SetSampleTime( object, sample->pts );
        if (sample->has_duration) IMFSample_SetSampleDuration( object, sample->duration );
        if (!sample->delta) IMFSample_SetUINT32( object, &MFSampleExtension_CleanPoint, 1 );
        IMFSample_Release( object );
    }

    if ((buffer->rtTimestamp = sample->pts) != INT64_MIN) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIME;
    if ((buffer->rtTimelength = sample->duration) != INT64_MIN) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH;
    if (!sample->delta) buffer->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT;
}

NTSTATUS CDECL winedmo_demuxer_read(struct winedmo_demuxer demuxer, UINT *stream, DMO_OUTPUT_DATA_BUFFER *buffer, UINT *buffer_size)
{
    struct demuxer *object = demuxer_from_handle(demuxer);
    struct wg_parser_buffer wg_buffer;
    NTSTATUS status = STATUS_SUCCESS;
    wg_parser_stream_t wg_stream;
    BYTE *data;
    DWORD size;

    TRACE("demuxer %#I64x, stream %p, buffer %p, buffer_size %p\n", demuxer.handle, stream, buffer, buffer_size);

    if (!wg_parser_stream_get_buffer(object->wg_parser, 0, &wg_buffer))
        return STATUS_END_OF_FILE;
    *buffer_size = wg_buffer.size;
    *stream = wg_buffer.stream;

    if (SUCCEEDED(IMediaBuffer_GetMaxLength( buffer->pBuffer, &size )) && size < wg_buffer.size)
        return STATUS_BUFFER_TOO_SMALL;

    buffer_lock(buffer, &data, &size);
    wg_stream = wg_parser_get_stream(object->wg_parser, wg_buffer.stream);
    if (!wg_parser_stream_copy_buffer(wg_stream, data, 0, wg_buffer.size))
    {
        status = STATUS_UNSUCCESSFUL;
        size = 0;
    }
    wg_parser_stream_release_buffer(wg_stream);
    buffer_unlock(buffer, &wg_buffer);

    TRACE("Got buffer %p, buffer_size %#x on stream %u\n", buffer->pBuffer, *buffer_size, *stream);
    return status;
}

NTSTATUS CDECL winedmo_demuxer_seek(struct winedmo_demuxer demuxer, INT64 timestamp)
{
    struct demuxer *object = demuxer_from_handle(demuxer);
    wg_parser_stream_t wg_stream;

    TRACE("demuxer %#I64x, timestamp %I64d\n", demuxer.handle, timestamp);

    wg_stream = wg_parser_get_stream(object->wg_parser, 0);
    wg_parser_stream_seek(wg_stream, 1.0, timestamp, 0, AM_SEEKING_AbsolutePositioning, AM_SEEKING_NoPositioning);
    return STATUS_SUCCESS;
}

NTSTATUS CDECL winedmo_demuxer_stream_lang(struct winedmo_demuxer demuxer, UINT stream, WCHAR *buffer, UINT len)
{
    struct demuxer *object = demuxer_from_handle(demuxer);
    wg_parser_stream_t wg_stream;
    char *tag;

    TRACE("demuxer %#I64x, stream %u\n", demuxer.handle, stream);

    wg_stream = wg_parser_get_stream(object->wg_parser, stream);
    if (!(tag = wg_parser_stream_get_tag(wg_stream, WG_PARSER_TAG_LANGUAGE))) return STATUS_UNSUCCESSFUL;
    len = MultiByteToWideChar(CP_UTF8, 0, tag, -1, buffer, len);
    buffer[len - 1] = 0;
    free(tag);

    return STATUS_SUCCESS;
}

NTSTATUS CDECL winedmo_demuxer_stream_name(struct winedmo_demuxer demuxer, UINT stream, WCHAR *buffer, UINT len)
{
    struct demuxer *object = demuxer_from_handle(demuxer);
    wg_parser_stream_t wg_stream;
    char *tag;

    TRACE("demuxer %#I64x, stream %u\n", demuxer.handle, stream);

    wg_stream = wg_parser_get_stream(object->wg_parser, stream);
    if (!(tag = wg_parser_stream_get_tag(wg_stream, WG_PARSER_TAG_NAME))) return STATUS_UNSUCCESSFUL;
    len = MultiByteToWideChar(CP_UTF8, 0, tag, -1, buffer, len);
    buffer[len - 1] = 0;
    free(tag);

    return STATUS_SUCCESS;
}

NTSTATUS CDECL winedmo_demuxer_stream_type(struct winedmo_demuxer demuxer, UINT stream, GUID *major, union winedmo_format **format)
{
    struct demuxer *object = demuxer_from_handle(demuxer);
    struct wg_media_type wg_media_type;
    wg_parser_stream_t wg_stream;
    struct wg_format wg_format;
    IMFMediaType *media_type;
    HRESULT hr;

    TRACE("demuxer %#I64x, stream %u, major %p, format %p\n", demuxer.handle, stream, major, format);

    wg_stream = wg_parser_get_stream(object->wg_parser, stream);
    wg_parser_stream_get_current_format(wg_stream, &wg_format);

    if (!(media_type = mf_media_type_from_wg_format(&wg_format)))
        return STATUS_NO_MEMORY;
    hr = wg_media_type_from_mf(media_type, &wg_media_type);
    IMFMediaType_Release(media_type);
    if (FAILED(hr))
        return STATUS_UNSUCCESSFUL;

    if (!(*format = calloc(1, wg_media_type.format_size)))
    {
        CoTaskMemFree(wg_media_type.u.format);
        return STATUS_NO_MEMORY;
    }

    *major = wg_media_type.major;
    memcpy(*format, wg_media_type.u.format, wg_media_type.format_size);
    CoTaskMemFree(wg_media_type.u.format);
    return STATUS_SUCCESS;
}
