/*
 * Copyright 2012 Austin English
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

#include "gst_private.h"
#include "dmoreg.h"
#include "mediaerr.h"

#include "initguid.h"
#include "nserror.h"
#include "winerror.h"
#include "wmsdk.h"

WINE_DEFAULT_DEBUG_CHANNEL(wmvcore);

struct wm_stream
{
    struct wm_reader *reader;
    wg_parser_stream_t wg_stream;
    struct wg_format compressed_format;
    AM_MEDIA_TYPE format;
    WMT_STREAM_SELECTION selection;
    /* NULL if outputting compressed samples */
    IMediaObject *decoder;
    WORD index;
    DWORD output_format_count;
    bool eos;
    bool read_compressed;

    DWORD sample_size;

    IWMReaderAllocatorEx *output_allocator;
    IWMReaderAllocatorEx *stream_allocator;
};

struct wm_reader
{
    IUnknown IUnknown_inner;
    IWMSyncReader2 IWMSyncReader2_iface;
    IWMHeaderInfo3 IWMHeaderInfo3_iface;
    IWMLanguageList IWMLanguageList_iface;
    IWMPacketSize2 IWMPacketSize2_iface;
    IWMProfile3 IWMProfile3_iface;
    IWMReaderPlaylistBurn IWMReaderPlaylistBurn_iface;
    IWMReaderTimecode IWMReaderTimecode_iface;
    IUnknown *outer;
    LONG refcount;

    CRITICAL_SECTION cs;
    CRITICAL_SECTION shutdown_cs;
    QWORD start_time;
    QWORD file_size;

    WCHAR *filename;
    IStream *source_stream;
    HANDLE file;
    HANDLE read_thread;
    HANDLE read_sem;
    bool read_thread_shutdown;
    wg_parser_t wg_parser;

    struct wm_stream *streams;
    WORD stream_count;
};

static struct wm_stream *get_stream_by_output_number(struct wm_reader *reader, DWORD output)
{
    if (output < reader->stream_count)
        return &reader->streams[output];
    WARN("Invalid output number %lu.\n", output);
    return NULL;
}

struct output_props
{
    IWMOutputMediaProps IWMOutputMediaProps_iface;
    LONG refcount;

    AM_MEDIA_TYPE mt;
};

static inline struct output_props *impl_from_IWMOutputMediaProps(IWMOutputMediaProps *iface)
{
    return CONTAINING_RECORD(iface, struct output_props, IWMOutputMediaProps_iface);
}

static HRESULT WINAPI output_props_QueryInterface(IWMOutputMediaProps *iface, REFIID iid, void **out)
{
    struct output_props *props = impl_from_IWMOutputMediaProps(iface);

    TRACE("props %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IWMOutputMediaProps))
        *out = &props->IWMOutputMediaProps_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI output_props_AddRef(IWMOutputMediaProps *iface)
{
    struct output_props *props = impl_from_IWMOutputMediaProps(iface);
    ULONG refcount = InterlockedIncrement(&props->refcount);

    TRACE("%p increasing refcount to %lu.\n", props, refcount);

    return refcount;
}

static ULONG WINAPI output_props_Release(IWMOutputMediaProps *iface)
{
    struct output_props *props = impl_from_IWMOutputMediaProps(iface);
    ULONG refcount = InterlockedDecrement(&props->refcount);

    TRACE("%p decreasing refcount to %lu.\n", props, refcount);

    if (!refcount)
    {
        FreeMediaType(&props->mt);
        free(props);
    }

    return refcount;
}

static HRESULT WINAPI output_props_GetType(IWMOutputMediaProps *iface, GUID *major_type)
{
    const struct output_props *props = impl_from_IWMOutputMediaProps(iface);

    TRACE("iface %p, major_type %p.\n", iface, major_type);

    *major_type = props->mt.majortype;
    return S_OK;
}

static HRESULT WINAPI output_props_GetMediaType(IWMOutputMediaProps *iface, WM_MEDIA_TYPE *mt, DWORD *size)
{
    const struct output_props *props = impl_from_IWMOutputMediaProps(iface);
    const DWORD req_size = *size;

    TRACE("iface %p, mt %p, size %p.\n", iface, mt, size);

    *size = sizeof(*mt) + props->mt.cbFormat;
    if (!mt)
        return S_OK;
    if (req_size < *size)
        return ASF_E_BUFFERTOOSMALL;

    strmbase_dump_media_type(&props->mt);

    memcpy(mt, &props->mt, sizeof(*mt));
    memcpy(mt + 1, props->mt.pbFormat, props->mt.cbFormat);
    mt->pbFormat = (BYTE *)(mt + 1);
    return S_OK;
}

static HRESULT WINAPI output_props_SetMediaType(IWMOutputMediaProps *iface, WM_MEDIA_TYPE *mt)
{
    const struct output_props *props = impl_from_IWMOutputMediaProps(iface);

    TRACE("iface %p, mt %p.\n", iface, mt);

    if (!mt)
        return E_POINTER;

    if (!IsEqualGUID(&props->mt.majortype, &mt->majortype))
        return E_FAIL;

    FreeMediaType((AM_MEDIA_TYPE *)&props->mt);
    return CopyMediaType((AM_MEDIA_TYPE *)&props->mt, (AM_MEDIA_TYPE *)mt);
}

static HRESULT WINAPI output_props_GetStreamGroupName(IWMOutputMediaProps *iface, WCHAR *name, WORD *len)
{
    FIXME("iface %p, name %p, len %p, stub!\n", iface, name, len);
    return E_NOTIMPL;
}

static HRESULT WINAPI output_props_GetConnectionName(IWMOutputMediaProps *iface, WCHAR *name, WORD *len)
{
    FIXME("iface %p, name %p, len %p, stub!\n", iface, name, len);
    return E_NOTIMPL;
}

static const struct IWMOutputMediaPropsVtbl output_props_vtbl =
{
    output_props_QueryInterface,
    output_props_AddRef,
    output_props_Release,
    output_props_GetType,
    output_props_GetMediaType,
    output_props_SetMediaType,
    output_props_GetStreamGroupName,
    output_props_GetConnectionName,
};

static IWMOutputMediaProps *output_props_create(const AM_MEDIA_TYPE *mt)
{
    struct output_props *object;

    if (!(object = calloc(1, sizeof(*object))))
        return NULL;
    object->IWMOutputMediaProps_iface.lpVtbl = &output_props_vtbl;
    object->refcount = 1;
    CopyMediaType(&object->mt, mt);

    TRACE("Created output properties %p.\n", object);
    return &object->IWMOutputMediaProps_iface;
}

struct buffer
{
    INSSBuffer INSSBuffer_iface;
    LONG refcount;

    DWORD size, capacity;
    BYTE data[1];
};

static struct buffer *impl_from_INSSBuffer(INSSBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct buffer, INSSBuffer_iface);
}

static HRESULT WINAPI buffer_QueryInterface(INSSBuffer *iface, REFIID iid, void **out)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_INSSBuffer))
        *out = &buffer->INSSBuffer_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI buffer_AddRef(INSSBuffer *iface)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);
    ULONG refcount = InterlockedIncrement(&buffer->refcount);

    TRACE("%p increasing refcount to %lu.\n", buffer, refcount);

    return refcount;
}

static ULONG WINAPI buffer_Release(INSSBuffer *iface)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);
    ULONG refcount = InterlockedDecrement(&buffer->refcount);

    TRACE("%p decreasing refcount to %lu.\n", buffer, refcount);

    if (!refcount)
        free(buffer);

    return refcount;
}

static HRESULT WINAPI buffer_GetLength(INSSBuffer *iface, DWORD *size)
{
    FIXME("iface %p, size %p, stub!\n", iface, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI buffer_SetLength(INSSBuffer *iface, DWORD size)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("iface %p, size %lu.\n", buffer, size);

    if (size > buffer->capacity)
        return E_INVALIDARG;

    buffer->size = size;
    return S_OK;
}

static HRESULT WINAPI buffer_GetMaxLength(INSSBuffer *iface, DWORD *size)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, size %p.\n", buffer, size);

    *size = buffer->capacity;
    return S_OK;
}

static HRESULT WINAPI buffer_GetBuffer(INSSBuffer *iface, BYTE **data)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, data %p.\n", buffer, data);

    *data = buffer->data;
    return S_OK;
}

static HRESULT WINAPI buffer_GetBufferAndLength(INSSBuffer *iface, BYTE **data, DWORD *size)
{
    struct buffer *buffer = impl_from_INSSBuffer(iface);

    TRACE("buffer %p, data %p, size %p.\n", buffer, data, size);

    *size = buffer->size;
    *data = buffer->data;
    return S_OK;
}

static const INSSBufferVtbl buffer_vtbl =
{
    buffer_QueryInterface,
    buffer_AddRef,
    buffer_Release,
    buffer_GetLength,
    buffer_SetLength,
    buffer_GetMaxLength,
    buffer_GetBuffer,
    buffer_GetBufferAndLength,
};

/* A non-owned wrapper of INSSBuffer that exposes an IMediaBuffer interface.
 * Only used for IMediaObject_ProcessOutput since it won't hold a reference to
 * the output buffer. */
struct media_buffer_wrapper {
    IMediaBuffer iface;
    INSSBuffer *inner;
    DWORD offset;
};

static struct media_buffer_wrapper *media_buffer_wrapper_from_IMediaBuffer(IMediaBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct media_buffer_wrapper, iface);
}

static HRESULT WINAPI media_buffer_wrapper_QueryInterface(IMediaBuffer *iface, REFIID iid, void **out)
{
    struct media_buffer_wrapper *buffer = media_buffer_wrapper_from_IMediaBuffer(iface);

    TRACE("buffer %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMediaBuffer))
        *out = &buffer->iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI media_buffer_wrapper_AddRef(IMediaBuffer *iface)
{
    return 2;
}

static ULONG WINAPI media_buffer_wrapper_Release(IMediaBuffer *iface)
{
    return 1;
}

static HRESULT WINAPI media_buffer_wrapper_SetLength(IMediaBuffer *iface, DWORD size)
{
    struct media_buffer_wrapper *buffer = media_buffer_wrapper_from_IMediaBuffer(iface);

    TRACE("iface %p, size %lu.\n", buffer, size);

    return INSSBuffer_SetLength(buffer->inner, size + buffer->offset);
}

static HRESULT WINAPI media_buffer_wrapper_GetMaxLength(IMediaBuffer *iface, DWORD *size)
{
    struct media_buffer_wrapper *buffer = media_buffer_wrapper_from_IMediaBuffer(iface);

    TRACE("buffer %p, size %p.\n", buffer, size);

    return INSSBuffer_GetMaxLength(buffer->inner, size - buffer->offset);
}

static HRESULT WINAPI media_buffer_wrapper_GetBufferAndLength(IMediaBuffer *iface, BYTE **data, DWORD *size)
{
    struct media_buffer_wrapper *buffer = media_buffer_wrapper_from_IMediaBuffer(iface);
    HRESULT hr;

    TRACE("buffer %p, data %p, size %p.\n", buffer, data, size);

    hr = INSSBuffer_GetBufferAndLength(buffer->inner, data, size);
    if (FAILED(hr)) return hr;

    *data += buffer->offset;
    *size -= buffer->offset;
    return hr;
}

static const IMediaBufferVtbl media_buffer_wrapper_vtbl =
{
    media_buffer_wrapper_QueryInterface,
    media_buffer_wrapper_AddRef,
    media_buffer_wrapper_Release,
    media_buffer_wrapper_SetLength,
    media_buffer_wrapper_GetMaxLength,
    media_buffer_wrapper_GetBufferAndLength,
};

struct media_buffer_wrapper wrap_nss_buffer(INSSBuffer *inner, DWORD offset)
{
    return (struct media_buffer_wrapper) {
        .inner = inner,
        .iface = { .lpVtbl = &media_buffer_wrapper_vtbl, },
    };
}

struct media_buffer {
    IMediaBuffer iface;

    LONG refcount;
    DWORD cap, size;
    BYTE data[1];
};

static struct media_buffer *media_buffer_from_IMediaBuffer(IMediaBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct media_buffer, iface);
}

static HRESULT WINAPI media_buffer_QueryInterface(IMediaBuffer *iface, REFIID iid, void **out)
{
    struct media_buffer *buffer = media_buffer_from_IMediaBuffer(iface);

    TRACE("buffer %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMediaBuffer))
        *out = &buffer->iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI media_buffer_AddRef(IMediaBuffer *iface)
{
    struct media_buffer *this = media_buffer_from_IMediaBuffer(iface);
    ULONG refcount = InterlockedIncrement(&this->refcount);
    TRACE("(%p) = %lu\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI media_buffer_Release(IMediaBuffer *iface)
{
    struct media_buffer *this = media_buffer_from_IMediaBuffer(iface);
    ULONG refcount = InterlockedDecrement(&this->refcount);
    TRACE("(%p) = %lu\n", iface, refcount);
    if (!refcount)
        free(this);
    return refcount;
}

static HRESULT WINAPI media_buffer_SetLength(IMediaBuffer *iface, DWORD size)
{
    struct media_buffer *buffer = media_buffer_from_IMediaBuffer(iface);

    TRACE("iface %p, size %lu.\n", buffer, size);
    if (size > buffer->cap) return E_INVALIDARG;
    buffer->size = size;
    return S_OK;
}

static HRESULT WINAPI media_buffer_GetMaxLength(IMediaBuffer *iface, DWORD *size)
{
    struct media_buffer *buffer = media_buffer_from_IMediaBuffer(iface);

    TRACE("buffer %p, size %p.\n", buffer, size);

    *size = buffer->cap;
    return S_OK;
}

static HRESULT WINAPI media_buffer_GetBufferAndLength(IMediaBuffer *iface, BYTE **data, DWORD *size)
{
    struct media_buffer *buffer = media_buffer_from_IMediaBuffer(iface);

    TRACE("buffer %p, data %p, size %p.\n", buffer, data, size);

    *data = buffer->data;
    *size = buffer->size;
    return S_OK;
}

static const IMediaBufferVtbl media_buffer_vtbl =
{
    media_buffer_QueryInterface,
    media_buffer_AddRef,
    media_buffer_Release,
    media_buffer_SetLength,
    media_buffer_GetMaxLength,
    media_buffer_GetBufferAndLength,
};

static HRESULT make_media_buffer(DWORD size, struct media_buffer **out)
{
    struct media_buffer *buffer = calloc(1, offsetof(struct media_buffer, data[size]));
    if (!buffer) return E_OUTOFMEMORY;

    buffer->size = 0;
    buffer->cap = size;
    buffer->refcount = 1;
    buffer->iface.lpVtbl = &media_buffer_vtbl;

    *out = buffer;
    return S_OK;
}

struct stream_config
{
    IWMStreamConfig IWMStreamConfig_iface;
    IWMMediaProps IWMMediaProps_iface;
    LONG refcount;

    const struct wm_stream *stream;
};

static struct stream_config *impl_from_IWMStreamConfig(IWMStreamConfig *iface)
{
    return CONTAINING_RECORD(iface, struct stream_config, IWMStreamConfig_iface);
}

static HRESULT WINAPI stream_config_QueryInterface(IWMStreamConfig *iface, REFIID iid, void **out)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);

    TRACE("config %p, iid %s, out %p.\n", config, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IWMStreamConfig))
        *out = &config->IWMStreamConfig_iface;
    else if (IsEqualGUID(iid, &IID_IWMMediaProps))
        *out = &config->IWMMediaProps_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI stream_config_AddRef(IWMStreamConfig *iface)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);
    ULONG refcount = InterlockedIncrement(&config->refcount);

    TRACE("%p increasing refcount to %lu.\n", config, refcount);

    return refcount;
}

static ULONG WINAPI stream_config_Release(IWMStreamConfig *iface)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);
    ULONG refcount = InterlockedDecrement(&config->refcount);

    TRACE("%p decreasing refcount to %lu.\n", config, refcount);

    if (!refcount)
    {
        IWMProfile3_Release(&config->stream->reader->IWMProfile3_iface);
        free(config);
    }

    return refcount;
}

static HRESULT WINAPI stream_config_GetStreamType(IWMStreamConfig *iface, GUID *type)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);
    struct wm_reader *reader = config->stream->reader;

    TRACE("config %p, type %p.\n", config, type);

    EnterCriticalSection(&reader->cs);
    *type = config->stream->format.majortype;
    LeaveCriticalSection(&reader->cs);

    return S_OK;
}

static HRESULT WINAPI stream_config_GetStreamNumber(IWMStreamConfig *iface, WORD *number)
{
    struct stream_config *config = impl_from_IWMStreamConfig(iface);

    TRACE("config %p, number %p.\n", config, number);

    *number = config->stream->index + 1;
    return S_OK;
}

static HRESULT WINAPI stream_config_SetStreamNumber(IWMStreamConfig *iface, WORD number)
{
    FIXME("iface %p, number %u, stub!\n", iface, number);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_GetStreamName(IWMStreamConfig *iface, WCHAR *name, WORD *len)
{
    FIXME("iface %p, name %p, len %p, stub!\n", iface, name, len);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_SetStreamName(IWMStreamConfig *iface, const WCHAR *name)
{
    FIXME("iface %p, name %s, stub!\n", iface, debugstr_w(name));
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_GetConnectionName(IWMStreamConfig *iface, WCHAR *name, WORD *len)
{
    FIXME("iface %p, name %p, len %p, stub!\n", iface, name, len);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_SetConnectionName(IWMStreamConfig *iface, const WCHAR *name)
{
    FIXME("iface %p, name %s, stub!\n", iface, debugstr_w(name));
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_GetBitrate(IWMStreamConfig *iface, DWORD *bitrate)
{
    FIXME("iface %p, bitrate %p, stub!\n", iface, bitrate);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_SetBitrate(IWMStreamConfig *iface, DWORD bitrate)
{
    FIXME("iface %p, bitrate %lu, stub!\n", iface, bitrate);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_GetBufferWindow(IWMStreamConfig *iface, DWORD *window)
{
    FIXME("iface %p, window %p, stub!\n", iface, window);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_config_SetBufferWindow(IWMStreamConfig *iface, DWORD window)
{
    FIXME("iface %p, window %lu, stub!\n", iface, window);
    return E_NOTIMPL;
}

static const IWMStreamConfigVtbl stream_config_vtbl =
{
    stream_config_QueryInterface,
    stream_config_AddRef,
    stream_config_Release,
    stream_config_GetStreamType,
    stream_config_GetStreamNumber,
    stream_config_SetStreamNumber,
    stream_config_GetStreamName,
    stream_config_SetStreamName,
    stream_config_GetConnectionName,
    stream_config_SetConnectionName,
    stream_config_GetBitrate,
    stream_config_SetBitrate,
    stream_config_GetBufferWindow,
    stream_config_SetBufferWindow,
};

static struct stream_config *impl_from_IWMMediaProps(IWMMediaProps *iface)
{
    return CONTAINING_RECORD(iface, struct stream_config, IWMMediaProps_iface);
}

static HRESULT WINAPI stream_props_QueryInterface(IWMMediaProps *iface, REFIID iid, void **out)
{
    struct stream_config *config = impl_from_IWMMediaProps(iface);
    return IWMStreamConfig_QueryInterface(&config->IWMStreamConfig_iface, iid, out);
}

static ULONG WINAPI stream_props_AddRef(IWMMediaProps *iface)
{
    struct stream_config *config = impl_from_IWMMediaProps(iface);
    return IWMStreamConfig_AddRef(&config->IWMStreamConfig_iface);
}

static ULONG WINAPI stream_props_Release(IWMMediaProps *iface)
{
    struct stream_config *config = impl_from_IWMMediaProps(iface);
    return IWMStreamConfig_Release(&config->IWMStreamConfig_iface);
}

static HRESULT WINAPI stream_props_GetType(IWMMediaProps *iface, GUID *major_type)
{
    FIXME("iface %p, major_type %p, stub!\n", iface, major_type);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_props_GetMediaType(IWMMediaProps *iface, WM_MEDIA_TYPE *mt, DWORD *size)
{
    struct stream_config *config = impl_from_IWMMediaProps(iface);
    const DWORD req_size = *size;
    AM_MEDIA_TYPE stream_mt;

    TRACE("iface %p, mt %p, size %p.\n", iface, mt, size);

    if (!amt_from_wg_format(&stream_mt, &config->stream->compressed_format, true))
        return E_OUTOFMEMORY;

    *size = sizeof(stream_mt) + stream_mt.cbFormat;
    if (mt && req_size >= *size)
    {
        strmbase_dump_media_type(&stream_mt);

        memcpy(mt, &stream_mt, sizeof(*mt));
        memcpy(mt + 1, stream_mt.pbFormat, stream_mt.cbFormat);
        mt->pbFormat = (BYTE *)(mt + 1);
    }
    FreeMediaType(&stream_mt);

    if (mt && req_size < *size)
        return ASF_E_BUFFERTOOSMALL;
    return S_OK;
}

static HRESULT WINAPI stream_props_SetMediaType(IWMMediaProps *iface, WM_MEDIA_TYPE *mt)
{
    FIXME("iface %p, mt %p, stub!\n", iface, mt);
    return E_NOTIMPL;
}

static const IWMMediaPropsVtbl stream_props_vtbl =
{
    stream_props_QueryInterface,
    stream_props_AddRef,
    stream_props_Release,
    stream_props_GetType,
    stream_props_GetMediaType,
    stream_props_SetMediaType,
};

static DWORD CALLBACK read_thread(void *arg)
{
    struct wm_reader *reader = arg;
    IStream *stream = reader->source_stream;
    HANDLE file = reader->file;
    size_t buffer_size = 4096;
    uint64_t file_size = reader->file_size;
    void *data;

    if (!(data = malloc(buffer_size)))
        return 0;

    TRACE("Starting read thread for reader %p.\n", reader);

    while (true)
    {
        LARGE_INTEGER large_offset;
        uint64_t offset;
        ULONG ret_size;
        uint32_t size;
        HRESULT hr;

        EnterCriticalSection(&reader->shutdown_cs);
        if (reader->read_thread_shutdown)
        {
            LeaveCriticalSection(&reader->shutdown_cs);
            break;
        }
        LeaveCriticalSection(&reader->shutdown_cs);

        if (!wg_parser_get_next_read_offset(reader->wg_parser, &offset, &size))
            continue;

        if (offset >= file_size)
            size = 0;
        else if (offset + size >= file_size)
            size = file_size - offset;

        if (!size)
        {
            wg_parser_push_data(reader->wg_parser, data, 0);
            continue;
        }

        if (!array_reserve(&data, &buffer_size, size, 1))
        {
            free(data);
            return 0;
        }

        ret_size = 0;
        hr = S_OK;
        large_offset.QuadPart = offset;

        WaitForSingleObject(reader->read_sem, INFINITE);
        if (file)
        {
            if (!SetFilePointerEx(file, large_offset, NULL, FILE_BEGIN)
                    || !ReadFile(file, data, size, &ret_size, NULL))
            {
                ERR("Failed to read %u bytes at offset %I64u, error %lu.\n", size, offset, GetLastError());
                wg_parser_push_data(reader->wg_parser, NULL, 0);
                hr = E_FAIL;
            }
        }
        else
        {
            if (SUCCEEDED(hr = IStream_Seek(stream, large_offset, STREAM_SEEK_SET, NULL)))
                hr = IStream_Read(stream, data, size, &ret_size);
            if (FAILED(hr))
            {
                ERR("Failed to read %u bytes at offset %I64u, hr %#lx.\n", size, offset, hr);
                wg_parser_push_data(reader->wg_parser, NULL, 0);
            }
        }
        ReleaseSemaphore(reader->read_sem, 1, NULL);
        if (FAILED(hr)) continue;

        if (ret_size != size)
            ERR("Unexpected short read: requested %u bytes, got %lu.\n", size, ret_size);
        wg_parser_push_data(reader->wg_parser, data, ret_size);
    }

    free(data);
    TRACE("Reader is shutting down; exiting.\n");
    return 0;
}

static struct wm_reader *impl_from_IWMProfile3(IWMProfile3 *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMProfile3_iface);
}

static HRESULT WINAPI profile_QueryInterface(IWMProfile3 *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI profile_AddRef(IWMProfile3 *iface)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI profile_Release(IWMProfile3 *iface)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI profile_GetVersion(IWMProfile3 *iface, WMT_VERSION *version)
{
    FIXME("iface %p, version %p, stub!\n", iface, version);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetName(IWMProfile3 *iface, WCHAR *name, DWORD *length)
{
    FIXME("iface %p, name %p, length %p, stub!\n", iface, name, length);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_SetName(IWMProfile3 *iface, const WCHAR *name)
{
    FIXME("iface %p, name %s, stub!\n", iface, debugstr_w(name));
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetDescription(IWMProfile3 *iface, WCHAR *description, DWORD *length)
{
    FIXME("iface %p, description %p, length %p, stub!\n", iface, description, length);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_SetDescription(IWMProfile3 *iface, const WCHAR *description)
{
    FIXME("iface %p, description %s, stub!\n", iface, debugstr_w(description));
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetStreamCount(IWMProfile3 *iface, DWORD *count)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);

    TRACE("reader %p, count %p.\n", reader, count);

    if (!count)
        return E_INVALIDARG;

    EnterCriticalSection(&reader->cs);
    *count = reader->stream_count;
    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI profile_GetStream(IWMProfile3 *iface, DWORD index, IWMStreamConfig **config)
{
    struct wm_reader *reader = impl_from_IWMProfile3(iface);
    struct stream_config *object;

    TRACE("reader %p, index %lu, config %p.\n", reader, index, config);

    EnterCriticalSection(&reader->cs);

    if (index >= reader->stream_count)
    {
        LeaveCriticalSection(&reader->cs);
        WARN("Index %lu exceeds stream count %u; returning E_INVALIDARG.\n", index, reader->stream_count);
        return E_INVALIDARG;
    }

    if (!(object = calloc(1, sizeof(*object))))
    {
        LeaveCriticalSection(&reader->cs);
        return E_OUTOFMEMORY;
    }

    object->IWMStreamConfig_iface.lpVtbl = &stream_config_vtbl;
    object->IWMMediaProps_iface.lpVtbl = &stream_props_vtbl;
    object->refcount = 1;
    object->stream = &reader->streams[index];
    IWMProfile3_AddRef(&reader->IWMProfile3_iface);

    LeaveCriticalSection(&reader->cs);

    TRACE("Created stream config %p.\n", object);
    *config = &object->IWMStreamConfig_iface;
    return S_OK;
}

static HRESULT WINAPI profile_GetStreamByNumber(IWMProfile3 *iface, WORD stream_number, IWMStreamConfig **config)
{
    HRESULT hr;

    TRACE("iface %p, stream_number %u, config %p.\n", iface, stream_number, config);

    if (!stream_number)
        return NS_E_NO_STREAM;

    hr = profile_GetStream(iface, stream_number - 1, config);
    if (hr == E_INVALIDARG)
        hr = NS_E_NO_STREAM;

    return hr;
}

static HRESULT WINAPI profile_RemoveStream(IWMProfile3 *iface, IWMStreamConfig *config)
{
    FIXME("iface %p, config %p, stub!\n", iface, config);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_RemoveStreamByNumber(IWMProfile3 *iface, WORD stream_number)
{
    FIXME("iface %p, stream_number %u, stub!\n", iface, stream_number);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_AddStream(IWMProfile3 *iface, IWMStreamConfig *config)
{
    FIXME("iface %p, config %p, stub!\n", iface, config);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_ReconfigStream(IWMProfile3 *iface, IWMStreamConfig *config)
{
    FIXME("iface %p, config %p, stub!\n", iface, config);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_CreateNewStream(IWMProfile3 *iface, REFGUID type, IWMStreamConfig **config)
{
    FIXME("iface %p, type %s, config %p, stub!\n", iface, debugstr_guid(type), config);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetMutualExclusionCount(IWMProfile3 *iface, DWORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetMutualExclusion(IWMProfile3 *iface, DWORD index, IWMMutualExclusion **excl)
{
    FIXME("iface %p, index %lu, excl %p, stub!\n", iface, index, excl);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_RemoveMutualExclusion(IWMProfile3 *iface, IWMMutualExclusion *excl)
{
    FIXME("iface %p, excl %p, stub!\n", iface, excl);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_AddMutualExclusion(IWMProfile3 *iface, IWMMutualExclusion *excl)
{
    FIXME("iface %p, excl %p, stub!\n", iface, excl);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_CreateNewMutualExclusion(IWMProfile3 *iface, IWMMutualExclusion **excl)
{
    FIXME("iface %p, excl %p, stub!\n", iface, excl);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetProfileID(IWMProfile3 *iface, GUID *id)
{
    FIXME("iface %p, id %p, stub!\n", iface, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetStorageFormat(IWMProfile3 *iface, WMT_STORAGE_FORMAT *format)
{
    FIXME("iface %p, format %p, stub!\n", iface, format);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_SetStorageFormat(IWMProfile3 *iface, WMT_STORAGE_FORMAT format)
{
    FIXME("iface %p, format %#x, stub!\n", iface, format);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetBandwidthSharingCount(IWMProfile3 *iface, DWORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetBandwidthSharing(IWMProfile3 *iface, DWORD index, IWMBandwidthSharing **sharing)
{
    FIXME("iface %p, index %lu, sharing %p, stub!\n", iface, index, sharing);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_RemoveBandwidthSharing( IWMProfile3 *iface, IWMBandwidthSharing *sharing)
{
    FIXME("iface %p, sharing %p, stub!\n", iface, sharing);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_AddBandwidthSharing(IWMProfile3 *iface, IWMBandwidthSharing *sharing)
{
    FIXME("iface %p, sharing %p, stub!\n", iface, sharing);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_CreateNewBandwidthSharing( IWMProfile3 *iface, IWMBandwidthSharing **sharing)
{
    FIXME("iface %p, sharing %p, stub!\n", iface, sharing);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetStreamPrioritization(IWMProfile3 *iface, IWMStreamPrioritization **stream)
{
    FIXME("iface %p, stream %p, stub!\n", iface, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_SetStreamPrioritization(IWMProfile3 *iface, IWMStreamPrioritization *stream)
{
    FIXME("iface %p, stream %p, stub!\n", iface, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_RemoveStreamPrioritization(IWMProfile3 *iface)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_CreateNewStreamPrioritization(IWMProfile3 *iface, IWMStreamPrioritization **stream)
{
    FIXME("iface %p, stream %p, stub!\n", iface, stream);
    return E_NOTIMPL;
}

static HRESULT WINAPI profile_GetExpectedPacketCount(IWMProfile3 *iface, QWORD duration, QWORD *count)
{
    FIXME("iface %p, duration %s, count %p, stub!\n", iface, debugstr_time(duration), count);
    return E_NOTIMPL;
}

static const IWMProfile3Vtbl profile_vtbl =
{
    profile_QueryInterface,
    profile_AddRef,
    profile_Release,
    profile_GetVersion,
    profile_GetName,
    profile_SetName,
    profile_GetDescription,
    profile_SetDescription,
    profile_GetStreamCount,
    profile_GetStream,
    profile_GetStreamByNumber,
    profile_RemoveStream,
    profile_RemoveStreamByNumber,
    profile_AddStream,
    profile_ReconfigStream,
    profile_CreateNewStream,
    profile_GetMutualExclusionCount,
    profile_GetMutualExclusion,
    profile_RemoveMutualExclusion,
    profile_AddMutualExclusion,
    profile_CreateNewMutualExclusion,
    profile_GetProfileID,
    profile_GetStorageFormat,
    profile_SetStorageFormat,
    profile_GetBandwidthSharingCount,
    profile_GetBandwidthSharing,
    profile_RemoveBandwidthSharing,
    profile_AddBandwidthSharing,
    profile_CreateNewBandwidthSharing,
    profile_GetStreamPrioritization,
    profile_SetStreamPrioritization,
    profile_RemoveStreamPrioritization,
    profile_CreateNewStreamPrioritization,
    profile_GetExpectedPacketCount,
};

static struct wm_reader *impl_from_IWMHeaderInfo3(IWMHeaderInfo3 *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMHeaderInfo3_iface);
}

static HRESULT WINAPI header_info_QueryInterface(IWMHeaderInfo3 *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMHeaderInfo3(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI header_info_AddRef(IWMHeaderInfo3 *iface)
{
    struct wm_reader *reader = impl_from_IWMHeaderInfo3(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI header_info_Release(IWMHeaderInfo3 *iface)
{
    struct wm_reader *reader = impl_from_IWMHeaderInfo3(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI header_info_GetAttributeCount(IWMHeaderInfo3 *iface, WORD stream_number, WORD *count)
{
    FIXME("iface %p, stream_number %u, count %p, stub!\n", iface, stream_number, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeByIndex(IWMHeaderInfo3 *iface, WORD index, WORD *stream_number,
        WCHAR *name, WORD *name_len, WMT_ATTR_DATATYPE *type, BYTE *value, WORD *size)
{
    FIXME("iface %p, index %u, stream_number %p, name %p, name_len %p, type %p, value %p, size %p, stub!\n",
            iface, index, stream_number, name, name_len, type, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeByName(IWMHeaderInfo3 *iface, WORD *stream_number,
        const WCHAR *name, WMT_ATTR_DATATYPE *type, BYTE *value, WORD *size)
{
    struct wm_reader *reader = impl_from_IWMHeaderInfo3(iface);
    const WORD req_size = *size;

    TRACE("reader %p, stream_number %p, name %s, type %p, value %p, size %u.\n",
            reader, stream_number, debugstr_w(name), type, value, *size);

    if (!stream_number)
        return E_INVALIDARG;

    if (!wcscmp(name, L"Duration"))
    {
        QWORD duration;

        if (*stream_number)
        {
            WARN("Requesting duration for stream %u, returning ASF_E_NOTFOUND.\n", *stream_number);
            return ASF_E_NOTFOUND;
        }

        *size = sizeof(QWORD);
        if (!value)
        {
            *type = WMT_TYPE_QWORD;
            return S_OK;
        }
        if (req_size < *size)
            return ASF_E_BUFFERTOOSMALL;

        *type = WMT_TYPE_QWORD;
        EnterCriticalSection(&reader->cs);
        duration = wg_parser_stream_get_duration(wg_parser_get_stream(reader->wg_parser, 0));
        LeaveCriticalSection(&reader->cs);
        TRACE("Returning duration %s.\n", debugstr_time(duration));
        memcpy(value, &duration, sizeof(QWORD));
        return S_OK;
    }
    else if (!wcscmp(name, L"Seekable"))
    {
        if (*stream_number)
        {
            WARN("Requesting duration for stream %u, returning ASF_E_NOTFOUND.\n", *stream_number);
            return ASF_E_NOTFOUND;
        }

        *size = sizeof(BOOL);
        if (!value)
        {
            *type = WMT_TYPE_BOOL;
            return S_OK;
        }
        if (req_size < *size)
            return ASF_E_BUFFERTOOSMALL;

        *type = WMT_TYPE_BOOL;
        *(BOOL *)value = TRUE;
        return S_OK;
    }
    else
    {
        FIXME("Unknown attribute %s.\n", debugstr_w(name));
        return ASF_E_NOTFOUND;
    }
}

static HRESULT WINAPI header_info_SetAttribute(IWMHeaderInfo3 *iface, WORD stream_number,
        const WCHAR *name, WMT_ATTR_DATATYPE type, const BYTE *value, WORD size)
{
    FIXME("iface %p, stream_number %u, name %s, type %#x, value %p, size %u, stub!\n",
            iface, stream_number, debugstr_w(name), type, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetMarkerCount(IWMHeaderInfo3 *iface, WORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetMarker(IWMHeaderInfo3 *iface,
        WORD index, WCHAR *name, WORD *len, QWORD *time)
{
    FIXME("iface %p, index %u, name %p, len %p, time %p, stub!\n", iface, index, name, len, time);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_AddMarker(IWMHeaderInfo3 *iface, const WCHAR *name, QWORD time)
{
    FIXME("iface %p, name %s, time %s, stub!\n", iface, debugstr_w(name), debugstr_time(time));
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_RemoveMarker(IWMHeaderInfo3 *iface, WORD index)
{
    FIXME("iface %p, index %u, stub!\n", iface, index);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetScriptCount(IWMHeaderInfo3 *iface, WORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetScript(IWMHeaderInfo3 *iface, WORD index, WCHAR *type,
        WORD *type_len, WCHAR *command, WORD *command_len, QWORD *time)
{
    FIXME("iface %p, index %u, type %p, type_len %p, command %p, command_len %p, time %p, stub!\n",
            iface, index, type, type_len, command, command_len, time);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_AddScript(IWMHeaderInfo3 *iface,
        const WCHAR *type, const WCHAR *command, QWORD time)
{
    FIXME("iface %p, type %s, command %s, time %s, stub!\n",
            iface, debugstr_w(type), debugstr_w(command), debugstr_time(time));
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_RemoveScript(IWMHeaderInfo3 *iface, WORD index)
{
    FIXME("iface %p, index %u, stub!\n", iface, index);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetCodecInfoCount(IWMHeaderInfo3 *iface, DWORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetCodecInfo(IWMHeaderInfo3 *iface, DWORD index, WORD *name_len,
        WCHAR *name, WORD *desc_len, WCHAR *desc, WMT_CODEC_INFO_TYPE *type, WORD *size, BYTE *info)
{
    FIXME("iface %p, index %lu, name_len %p, name %p, desc_len %p, desc %p, type %p, size %p, info %p, stub!\n",
            iface, index, name_len, name, desc_len, desc, type, size, info);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeCountEx(IWMHeaderInfo3 *iface, WORD stream_number, WORD *count)
{
    FIXME("iface %p, stream_number %u, count %p, stub!\n", iface, stream_number, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeIndices(IWMHeaderInfo3 *iface, WORD stream_number,
        const WCHAR *name, WORD *lang_index, WORD *indices, WORD *count)
{
    FIXME("iface %p, stream_number %u, name %s, lang_index %p, indices %p, count %p, stub!\n",
            iface, stream_number, debugstr_w(name), lang_index, indices, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_GetAttributeByIndexEx(IWMHeaderInfo3 *iface,
        WORD stream_number, WORD index, WCHAR *name, WORD *name_len,
        WMT_ATTR_DATATYPE *type, WORD *lang_index, BYTE *value, DWORD *size)
{
    FIXME("iface %p, stream_number %u, index %u, name %p, name_len %p,"
            " type %p, lang_index %p, value %p, size %p, stub!\n",
            iface, stream_number, index, name, name_len, type, lang_index, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_ModifyAttribute(IWMHeaderInfo3 *iface, WORD stream_number,
        WORD index, WMT_ATTR_DATATYPE type, WORD lang_index, const BYTE *value, DWORD size)
{
    FIXME("iface %p, stream_number %u, index %u, type %#x, lang_index %u, value %p, size %lu, stub!\n",
            iface, stream_number, index, type, lang_index, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_AddAttribute(IWMHeaderInfo3 *iface,
        WORD stream_number, const WCHAR *name, WORD *index,
        WMT_ATTR_DATATYPE type, WORD lang_index, const BYTE *value, DWORD size)
{
    FIXME("iface %p, stream_number %u, name %s, index %p, type %#x, lang_index %u, value %p, size %lu, stub!\n",
            iface, stream_number, debugstr_w(name), index, type, lang_index, value, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_DeleteAttribute(IWMHeaderInfo3 *iface, WORD stream_number, WORD index)
{
    FIXME("iface %p, stream_number %u, index %u, stub!\n", iface, stream_number, index);
    return E_NOTIMPL;
}

static HRESULT WINAPI header_info_AddCodecInfo(IWMHeaderInfo3 *iface, const WCHAR *name,
        const WCHAR *desc, WMT_CODEC_INFO_TYPE type, WORD size, BYTE *info)
{
    FIXME("iface %p, name %s, desc %s, type %#x, size %u, info %p, stub!\n",
            info, debugstr_w(name), debugstr_w(desc), type, size, info);
    return E_NOTIMPL;
}

static const IWMHeaderInfo3Vtbl header_info_vtbl =
{
    header_info_QueryInterface,
    header_info_AddRef,
    header_info_Release,
    header_info_GetAttributeCount,
    header_info_GetAttributeByIndex,
    header_info_GetAttributeByName,
    header_info_SetAttribute,
    header_info_GetMarkerCount,
    header_info_GetMarker,
    header_info_AddMarker,
    header_info_RemoveMarker,
    header_info_GetScriptCount,
    header_info_GetScript,
    header_info_AddScript,
    header_info_RemoveScript,
    header_info_GetCodecInfoCount,
    header_info_GetCodecInfo,
    header_info_GetAttributeCountEx,
    header_info_GetAttributeIndices,
    header_info_GetAttributeByIndexEx,
    header_info_ModifyAttribute,
    header_info_AddAttribute,
    header_info_DeleteAttribute,
    header_info_AddCodecInfo,
};

static struct wm_reader *impl_from_IWMLanguageList(IWMLanguageList *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMLanguageList_iface);
}

static HRESULT WINAPI language_list_QueryInterface(IWMLanguageList *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMLanguageList(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI language_list_AddRef(IWMLanguageList *iface)
{
    struct wm_reader *reader = impl_from_IWMLanguageList(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI language_list_Release(IWMLanguageList *iface)
{
    struct wm_reader *reader = impl_from_IWMLanguageList(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI language_list_GetLanguageCount(IWMLanguageList *iface, WORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI language_list_GetLanguageDetails(IWMLanguageList *iface,
        WORD index, WCHAR *lang, WORD *len)
{
    FIXME("iface %p, index %u, lang %p, len %p, stub!\n", iface, index, lang, len);
    return E_NOTIMPL;
}

static HRESULT WINAPI language_list_AddLanguageByRFC1766String(IWMLanguageList *iface,
        const WCHAR *lang, WORD *index)
{
    FIXME("iface %p, lang %s, index %p, stub!\n", iface, debugstr_w(lang), index);
    return E_NOTIMPL;
}

static const IWMLanguageListVtbl language_list_vtbl =
{
    language_list_QueryInterface,
    language_list_AddRef,
    language_list_Release,
    language_list_GetLanguageCount,
    language_list_GetLanguageDetails,
    language_list_AddLanguageByRFC1766String,
};

static struct wm_reader *impl_from_IWMPacketSize2(IWMPacketSize2 *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMPacketSize2_iface);
}

static HRESULT WINAPI packet_size_QueryInterface(IWMPacketSize2 *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMPacketSize2(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI packet_size_AddRef(IWMPacketSize2 *iface)
{
    struct wm_reader *reader = impl_from_IWMPacketSize2(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI packet_size_Release(IWMPacketSize2 *iface)
{
    struct wm_reader *reader = impl_from_IWMPacketSize2(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI packet_size_GetMaxPacketSize(IWMPacketSize2 *iface, DWORD *size)
{
    FIXME("iface %p, size %p, stub!\n", iface, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI packet_size_SetMaxPacketSize(IWMPacketSize2 *iface, DWORD size)
{
    FIXME("iface %p, size %lu, stub!\n", iface, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI packet_size_GetMinPacketSize(IWMPacketSize2 *iface, DWORD *size)
{
    FIXME("iface %p, size %p, stub!\n", iface, size);
    return E_NOTIMPL;
}

static HRESULT WINAPI packet_size_SetMinPacketSize(IWMPacketSize2 *iface, DWORD size)
{
    FIXME("iface %p, size %lu, stub!\n", iface, size);
    return E_NOTIMPL;
}

static const IWMPacketSize2Vtbl packet_size_vtbl =
{
    packet_size_QueryInterface,
    packet_size_AddRef,
    packet_size_Release,
    packet_size_GetMaxPacketSize,
    packet_size_SetMaxPacketSize,
    packet_size_GetMinPacketSize,
    packet_size_SetMinPacketSize,
};

static struct wm_reader *impl_from_IWMReaderPlaylistBurn(IWMReaderPlaylistBurn *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMReaderPlaylistBurn_iface);
}

static HRESULT WINAPI playlist_QueryInterface(IWMReaderPlaylistBurn *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMReaderPlaylistBurn(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI playlist_AddRef(IWMReaderPlaylistBurn *iface)
{
    struct wm_reader *reader = impl_from_IWMReaderPlaylistBurn(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI playlist_Release(IWMReaderPlaylistBurn *iface)
{
    struct wm_reader *reader = impl_from_IWMReaderPlaylistBurn(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI playlist_InitPlaylistBurn(IWMReaderPlaylistBurn *iface, DWORD count,
        const WCHAR **filenames, IWMStatusCallback *callback, void *context)
{
    FIXME("iface %p, count %lu, filenames %p, callback %p, context %p, stub!\n",
            iface, count, filenames, callback, context);
    return E_NOTIMPL;
}

static HRESULT WINAPI playlist_GetInitResults(IWMReaderPlaylistBurn *iface, DWORD count, HRESULT *hrs)
{
    FIXME("iface %p, count %lu, hrs %p, stub!\n", iface, count, hrs);
    return E_NOTIMPL;
}

static HRESULT WINAPI playlist_Cancel(IWMReaderPlaylistBurn *iface)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI playlist_EndPlaylistBurn(IWMReaderPlaylistBurn *iface, HRESULT hr)
{
    FIXME("iface %p, hr %#lx, stub!\n", iface, hr);
    return E_NOTIMPL;
}

static const IWMReaderPlaylistBurnVtbl playlist_vtbl =
{
    playlist_QueryInterface,
    playlist_AddRef,
    playlist_Release,
    playlist_InitPlaylistBurn,
    playlist_GetInitResults,
    playlist_Cancel,
    playlist_EndPlaylistBurn,
};

static struct wm_reader *impl_from_IWMReaderTimecode(IWMReaderTimecode *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMReaderTimecode_iface);
}

static HRESULT WINAPI timecode_QueryInterface(IWMReaderTimecode *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMReaderTimecode(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI timecode_AddRef(IWMReaderTimecode *iface)
{
    struct wm_reader *reader = impl_from_IWMReaderTimecode(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI timecode_Release(IWMReaderTimecode *iface)
{
    struct wm_reader *reader = impl_from_IWMReaderTimecode(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI timecode_GetTimecodeRangeCount(IWMReaderTimecode *iface,
        WORD stream_number, WORD *count)
{
    FIXME("iface %p, stream_number %u, count %p, stub!\n", iface, stream_number, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI timecode_GetTimecodeRangeBounds(IWMReaderTimecode *iface,
        WORD stream_number, WORD index, DWORD *start, DWORD *end)
{
    FIXME("iface %p, stream_number %u, index %u, start %p, end %p, stub!\n",
            iface, stream_number, index, start, end);
    return E_NOTIMPL;
}

static const IWMReaderTimecodeVtbl timecode_vtbl =
{
    timecode_QueryInterface,
    timecode_AddRef,
    timecode_Release,
    timecode_GetTimecodeRangeCount,
    timecode_GetTimecodeRangeBounds,
};

static bool get_media_type_extent(const AM_MEDIA_TYPE *mt, WORD *width, WORD *height)
{
    if (IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
    {
        VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *)mt->pbFormat;
        *width = vih->bmiHeader.biWidth;
        *height = vih->bmiHeader.biHeight;
        return true;
    }
    if (IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo2))
    {
        VIDEOINFOHEADER2 *vih = (VIDEOINFOHEADER2 *)mt->pbFormat;
        *width = vih->bmiHeader.biWidth;
        *height = vih->bmiHeader.biHeight;
        return true;
    }
    return false;
}

static HRESULT find_uncompressed_media_type(IMediaObject *object, AM_MEDIA_TYPE *mt, DWORD *total, bool is_video)
{
    GUID target_subtype;
    HRESULT hr = NS_E_CODEC_UNAVAILABLE;
    DWORD i;
    if (is_video)
    {
        /* Call of Juarez: Bound in Blood breaks if I420 is enumerated.
         * Some native decoders output I420, but the msmpeg4v3 decoder
         * never does.
         *
         * Shadowgrounds provides wmv3 video and assumes that the initial
         * video type will be BGR. */
        target_subtype = MEDIASUBTYPE_RGB24;
    }
    else
    {
        /* R.U.S.E enumerates available audio types, picks the first one it
         * likes, and then sets the wrong stream to that type. libav might
         * give us WG_AUDIO_FORMAT_F32LE by default, which will result in
         * the game incorrectly interpreting float data as integer.
         * Therefore just match native and always set our default format to
         * S16LE. (bit depth is checked below) */
        target_subtype = MEDIASUBTYPE_PCM;
    }

    for (i = 0; ;i++)
    {
        hr = IMediaObject_GetOutputType(object, 0, i, mt);
        if (FAILED(hr)) break;
        if (IsEqualGUID(&mt->subtype, &target_subtype))
        {
            if (is_video) break;
            else if (IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx))
            {
                WAVEFORMATEX *wfex = (WAVEFORMATEX *)mt->pbFormat;
                if (wfex->wBitsPerSample == 16) break;
            }
        }
        FreeMediaType(mt);
    }

    if (SUCCEEDED(hr))
    {
        AM_MEDIA_TYPE mt2;
        do
        {
            i++;
            hr = IMediaObject_GetOutputType(object, 0, i, &mt2);
            if (FAILED(hr))
            {
                if (hr == DMO_E_NO_MORE_ITEMS)
                {
                    TRACE("total output types: %lu\n", i);
                    *total = i;
                    hr = S_OK;
                }
                break;
            }
            FreeMediaType(&mt2);
        } while (TRUE);
    }

    return hr;
}

static HRESULT find_and_create_decoder(struct wm_stream *stream)
{
    DWORD num_in_streams, num_out_streams;
    DMO_PARTIAL_MEDIATYPE in_type, out_type;
    LPWSTR decoder_name;
    CLSID decoder_clsid;
    IEnumDMO *it = NULL;
    DWORD num_decoders;
    DWORD alignment;
    GUID category;
    HRESULT hr;
    bool is_video;


    if (!amt_from_wg_format(&stream->format, &stream->compressed_format, true))
        return NS_E_CODEC_UNAVAILABLE;

    in_type.type = out_type.type = stream->format.majortype;
    in_type.subtype = stream->format.subtype;

    if (IsEqualGUID(&out_type.type, &MEDIATYPE_Video))
    {
        is_video = true;
        category = DMOCATEGORY_VIDEO_DECODER;
        out_type.subtype = MEDIASUBTYPE_RGB24;
    }
    else if (IsEqualGUID(&out_type.type, &MEDIATYPE_Audio))
    {
        is_video = false;
        category = DMOCATEGORY_AUDIO_DECODER;
        out_type.subtype = MEDIASUBTYPE_PCM;
    }
    else return NS_E_CODEC_UNAVAILABLE;

    hr = DMOEnum(&category, 0, 1, &in_type, 1, &out_type, &it);
    if (FAILED(hr))
    {
        WARN("Failed to enumerate DMOs %#lx\n", hr);
        goto err;
    }

    if (IEnumDMO_Next(it, 1, &decoder_clsid, &decoder_name, &num_decoders) != S_OK)
    {
        WARN("No decoder found\n");
        goto err;
    }

    hr = CoCreateInstance(&decoder_clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMediaObject, (void **)&stream->decoder);
    if (FAILED(hr))
    {
        WARN("Failed to create DMO %#lx\n", hr);
        goto err;
    }

    hr = IMediaObject_GetStreamCount(stream->decoder, &num_in_streams, &num_out_streams);
    if (FAILED(hr))
    {
        WARN("Failed to get DMO stream counts %#lx\n", hr);
        goto err;
    }
    if (num_in_streams != 1 || num_out_streams != 1)
    {
        WARN("Unexpected stream counts for decoder DMO\n");
        goto err;
    }

    hr = IMediaObject_SetInputType(stream->decoder, 0, &stream->format, 0);
    if (FAILED(hr))
    {
        WARN("Failed to set media type on DMO %#lx\n", hr);
        goto err;
    }

    FreeMediaType(&stream->format);
    hr = find_uncompressed_media_type(stream->decoder, &stream->format,
            &stream->output_format_count, is_video);
    if (FAILED(hr))
    {
        WARN("Failed to get media type\n");
        goto err;
    }

    hr = IMediaObject_SetOutputType(stream->decoder, 0, &stream->format, 0);
    if (FAILED(hr))
    {
        WARN("Failed to set DMO output type %#lx\n", hr);
        goto err;
    }

    /* FIXME: safe to assume IMediaObject_GetOutputCurrentType will return the same format we just set? */

    hr = IMediaObject_GetOutputSizeInfo(stream->decoder, 0, &stream->sample_size, &alignment);
    if (FAILED(hr))
    {
        WARN("Failed to get output size from DMO %#lx\n", hr);
        goto err;
    }
    return S_OK;

err:
    if (it) IEnumDMO_Release(it);
    if (stream->decoder) IMediaObject_Release(stream->decoder);
    FreeMediaType(&stream->format);
    return NS_E_CODEC_UNAVAILABLE;
}

static HRESULT init_stream(struct wm_reader *reader)
{
    wg_parser_t wg_parser;
    HRESULT hr;
    WORD i;

    if (!(wg_parser = wg_parser_create(TRUE)))
        return E_OUTOFMEMORY;

    reader->wg_parser = wg_parser;
    reader->read_thread_shutdown = false;

    if (!(reader->read_sem = CreateSemaphoreA(NULL, 1, LONG_MAX, NULL)))
    {
        hr = E_OUTOFMEMORY;
        goto out_destroy_parser;
    }

    if (!(reader->read_thread = CreateThread(NULL, 0, read_thread, reader, 0, NULL)))
    {
        hr = E_OUTOFMEMORY;
        goto out_destroy_parser;
    }

    if (FAILED(hr = wg_parser_connect(reader->wg_parser, reader->file_size, reader->filename)))
    {
        ERR("Failed to connect parser, hr %#lx.\n", hr);
        goto out_shutdown_thread;
    }

    reader->stream_count = wg_parser_get_stream_count(reader->wg_parser);

    if (!(reader->streams = calloc(reader->stream_count, sizeof(*reader->streams))))
    {
        hr = E_OUTOFMEMORY;
        goto out_disconnect_parser;
    }

    for (i = 0; i < reader->stream_count; ++i)
    {
        struct wm_stream *stream = &reader->streams[i];

        stream->wg_stream = wg_parser_get_stream(reader->wg_parser, i);
        stream->reader = reader;
        stream->index = i;
        stream->selection = WMT_ON;
        wg_parser_stream_get_current_format(stream->wg_stream, &stream->compressed_format);

        hr = find_and_create_decoder(stream);
        if (FAILED(hr))
            goto out_disconnect_parser;

        wg_parser_stream_enable(stream->wg_stream, &stream->compressed_format);
    }

    /* We probably discarded events because streams weren't enabled yet.
     * Now that they're all enabled seek back to the start again. */
    wg_parser_stream_seek(reader->streams[0].wg_stream, 1.0, 0, 0,
            AM_SEEKING_AbsolutePositioning, AM_SEEKING_NoPositioning);
    /* Pause the read thread */
    if (WaitForSingleObject(reader->read_sem, INFINITE) != WAIT_OBJECT_0)
    {
        ERR("Failed to wait for read semaphore.\n");
        goto out_disconnect_parser;
    }
    return S_OK;

out_disconnect_parser:
    wg_parser_disconnect(reader->wg_parser);

out_shutdown_thread:
    EnterCriticalSection(&reader->shutdown_cs);
    reader->read_thread_shutdown = true;
    LeaveCriticalSection(&reader->shutdown_cs);
    WaitForSingleObject(reader->read_thread, INFINITE);
    CloseHandle(reader->read_thread);
    reader->read_thread = NULL;

out_destroy_parser:
    if (reader->read_sem)
    {
        CloseHandle(reader->read_sem);
        reader->read_sem = NULL;
    }
    wg_parser_destroy(reader->wg_parser);
    reader->wg_parser = 0;

    return hr;
}

static struct wm_stream *wm_reader_get_stream_by_stream_number(struct wm_reader *reader, WORD stream_number)
{
    if (stream_number && stream_number <= reader->stream_count)
        return &reader->streams[stream_number - 1];
    WARN("Invalid stream number %u.\n", stream_number);
    return NULL;
}

static const char *get_major_type_string(enum wg_major_type type)
{
    switch (type)
    {
        case WG_MAJOR_TYPE_AUDIO:
            return "audio";
        case WG_MAJOR_TYPE_AUDIO_MPEG1:
            return "mpeg1-audio";
        case WG_MAJOR_TYPE_AUDIO_MPEG4:
            return "mpeg4-audio";
        case WG_MAJOR_TYPE_AUDIO_WMA:
            return "wma";
        case WG_MAJOR_TYPE_VIDEO:
            return "video";
        case WG_MAJOR_TYPE_VIDEO_CINEPAK:
            return "cinepak";
        case WG_MAJOR_TYPE_VIDEO_H264:
            return "h264";
        case WG_MAJOR_TYPE_VIDEO_WMV:
            return "wmv";
        case WG_MAJOR_TYPE_VIDEO_INDEO:
            return "indeo";
        case WG_MAJOR_TYPE_VIDEO_MPEG1:
            return "mpeg1-video";
        case WG_MAJOR_TYPE_UNKNOWN:
            return "unknown";
    }
    assert(0);
    return NULL;
}

static HRESULT wm_stream_allocate_sample(struct wm_stream *stream, DWORD size, INSSBuffer **sample)
{
    struct buffer *buffer;

    if (!stream->read_compressed && stream->output_allocator)
        return IWMReaderAllocatorEx_AllocateForOutputEx(stream->output_allocator, stream->index,
                size, sample, 0, 0, 0, NULL);

    if (stream->read_compressed && stream->stream_allocator)
        return IWMReaderAllocatorEx_AllocateForStreamEx(stream->stream_allocator, stream->index + 1,
                size, sample, 0, 0, 0, NULL);

    /* FIXME: Should these be pooled? */
    if (!(buffer = calloc(1, offsetof(struct buffer, data[size]))))
        return E_OUTOFMEMORY;
    buffer->INSSBuffer_iface.lpVtbl = &buffer_vtbl;
    buffer->refcount = 1;
    buffer->capacity = size;

    TRACE("Created buffer %p, size %lu.\n", buffer, size);
    *sample = &buffer->INSSBuffer_iface;
    return S_OK;
}

static struct wm_reader *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IUnknown_inner);
}

static HRESULT WINAPI unknown_inner_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IUnknown(iface);

    TRACE("reader %p, iid %s, out %p.\n", reader, debugstr_guid(iid), out);

    if (IsEqualIID(iid, &IID_IUnknown)
            || IsEqualIID(iid, &IID_IWMSyncReader)
            || IsEqualIID(iid, &IID_IWMSyncReader2))
        *out = &reader->IWMSyncReader2_iface;
    else if (IsEqualIID(iid, &IID_IWMHeaderInfo)
            || IsEqualIID(iid, &IID_IWMHeaderInfo2)
            || IsEqualIID(iid, &IID_IWMHeaderInfo3))
        *out = &reader->IWMHeaderInfo3_iface;
    else if (IsEqualIID(iid, &IID_IWMLanguageList))
        *out = &reader->IWMLanguageList_iface;
    else if (IsEqualIID(iid, &IID_IWMPacketSize)
            || IsEqualIID(iid, &IID_IWMPacketSize2))
        *out = &reader->IWMPacketSize2_iface;
    else if (IsEqualIID(iid, &IID_IWMProfile)
            || IsEqualIID(iid, &IID_IWMProfile2)
            || IsEqualIID(iid, &IID_IWMProfile3))
        *out = &reader->IWMProfile3_iface;
    else if (IsEqualIID(iid, &IID_IWMReaderPlaylistBurn))
        *out = &reader->IWMReaderPlaylistBurn_iface;
    else if (IsEqualIID(iid, &IID_IWMReaderTimecode))
        *out = &reader->IWMReaderTimecode_iface;
    else
    {
        FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI unknown_inner_AddRef(IUnknown *iface)
{
    struct wm_reader *reader = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&reader->refcount);
    TRACE("%p increasing refcount to %lu.\n", reader, refcount);
    return refcount;
}

static ULONG WINAPI unknown_inner_Release(IUnknown *iface)
{
    struct wm_reader *reader = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&reader->refcount);

    TRACE("%p decreasing refcount to %lu.\n", reader, refcount);

    if (!refcount)
    {
        IWMSyncReader2_Close(&reader->IWMSyncReader2_iface);

        reader->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&reader->cs);
        reader->shutdown_cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&reader->shutdown_cs);

        free(reader);
    }

    return refcount;
}

static const IUnknownVtbl unknown_inner_vtbl =
{
    unknown_inner_QueryInterface,
    unknown_inner_AddRef,
    unknown_inner_Release,
};

static struct wm_reader *impl_from_IWMSyncReader2(IWMSyncReader2 *iface)
{
    return CONTAINING_RECORD(iface, struct wm_reader, IWMSyncReader2_iface);
}

static HRESULT WINAPI reader_QueryInterface(IWMSyncReader2 *iface, REFIID iid, void **out)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    return IUnknown_QueryInterface(reader->outer, iid, out);
}

static ULONG WINAPI reader_AddRef(IWMSyncReader2 *iface)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    return IUnknown_AddRef(reader->outer);
}

static ULONG WINAPI reader_Release(IWMSyncReader2 *iface)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    return IUnknown_Release(reader->outer);
}

static HRESULT WINAPI reader_Close(IWMSyncReader2 *iface)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p.\n", reader);

    EnterCriticalSection(&reader->cs);

    if (!reader->wg_parser)
    {
        LeaveCriticalSection(&reader->cs);
        return NS_E_INVALID_REQUEST;
    }

    ReleaseSemaphore(reader->read_sem, 1, NULL);

    wg_parser_disconnect(reader->wg_parser);

    EnterCriticalSection(&reader->shutdown_cs);
    reader->read_thread_shutdown = true;
    LeaveCriticalSection(&reader->shutdown_cs);
    WaitForSingleObject(reader->read_thread, INFINITE);
    CloseHandle(reader->read_thread);
    CloseHandle(reader->read_sem);
    reader->read_thread = NULL;
    reader->read_sem = NULL;

    wg_parser_destroy(reader->wg_parser);
    reader->wg_parser = 0;

    if (reader->source_stream)
        IStream_Release(reader->source_stream);
    reader->source_stream = NULL;
    if (reader->file)
        CloseHandle(reader->file);
    reader->file = NULL;
    free(reader->filename);
    reader->filename = NULL;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetMaxOutputSampleSize(IWMSyncReader2 *iface, DWORD output_number, DWORD *size)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output_number %lu, size %p.\n", reader, output_number, size);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if (stream->read_compressed) *size = wg_format_get_max_size(&stream->compressed_format);
    else *size = stream->sample_size;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetMaxStreamSampleSize(IWMSyncReader2 *iface, WORD stream_number, DWORD *size)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %u, size %p.\n", reader, stream_number, size);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    *size = wg_format_get_max_size(&stream->compressed_format);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT media_object_read_all(struct wm_stream *stream, INSSBuffer **sample, QWORD *pts, QWORD *duration)
{
    DWORD alloc_size = stream->sample_size;
    struct media_buffer_wrapper media_buf;
    DMO_OUTPUT_DATA_BUFFER dmo_buf;
    DWORD buf_offset = 0;
    INSSBuffer *buf = *sample;
    HRESULT hr;

    TRACE("stream %p sample %p, buf %p alloc_size %lu\n", stream, sample, buf, alloc_size);

    if (!buf)
    {
        hr = wm_stream_allocate_sample(stream, alloc_size, &buf);
        if (FAILED(hr)) return NS_E_NO_MORE_SAMPLES;
        hr = INSSBuffer_SetLength(buf, 0);
        if (FAILED(hr)) return hr;
    }
    else
    {
        BYTE *data;
        hr = INSSBuffer_GetBufferAndLength(buf, &data, &buf_offset);
        if (FAILED(hr)) return hr;
        TRACE("%lu\n", buf_offset);
        assert(buf_offset == 0);

        hr = INSSBuffer_GetMaxLength(buf, &alloc_size);
        if (FAILED(hr)) return hr;
    }

    while (TRUE) {
        INSSBuffer *bigger_buf;
        DWORD size, larger_size;
        BYTE *data, *more_data;
        DWORD status;

        memset(&dmo_buf, 0, sizeof(dmo_buf));
        media_buf = wrap_nss_buffer(buf, buf_offset);
        dmo_buf.pBuffer = &media_buf.iface;

        hr = IMediaObject_ProcessOutput(stream->decoder, 0, 1, &dmo_buf, &status);
        if (FAILED(hr) || hr == S_FALSE) break;

        if (buf_offset == 0 && (dmo_buf.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIME))
            *pts = dmo_buf.rtTimestamp;
        if (dmo_buf.dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH)
            *duration += dmo_buf.rtTimelength;
        if (!(dmo_buf.dwStatus & DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE)) break;

        hr = INSSBuffer_GetBufferAndLength(buf, &data, &size);
        if (FAILED(hr)) break;

        alloc_size *= 2;
        hr = wm_stream_allocate_sample(stream, alloc_size, &bigger_buf);
        if (FAILED(hr)) break;

        hr = INSSBuffer_GetBufferAndLength(bigger_buf, &more_data, &larger_size);
        TRACE("larger size %lu\n", larger_size);
        if (FAILED(hr))
        {
            INSSBuffer_Release(bigger_buf);
            break;
        }

        memcpy(more_data, data, size);
        INSSBuffer_SetLength(bigger_buf, size);

        INSSBuffer_Release(buf);
        buf_offset = size;
        buf = bigger_buf;
    }

    TRACE("Returning sample %p, hr %#lx, buf_offset %lu\n", buf, hr, buf_offset);
    *sample = buf;
    if (hr == S_FALSE && buf_offset) hr = S_OK;
    return hr;
}

static HRESULT WINAPI reader_GetNextSample(IWMSyncReader2 *iface,
        WORD stream_number, INSSBuffer **sample, QWORD *pts, QWORD *duration,
        DWORD *out_flags, DWORD *output_number, WORD *ret_stream_number)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream, *last_read_stream = NULL;
    HRESULT hr = S_FALSE;
    INSSBuffer *buf = NULL;
    DWORD i;

    TRACE("reader %p, stream_number %u, sample %p, pts %p, duration %p,"
            " flags %p, output_number %p, ret_stream_number %p.\n",
            reader, stream_number, sample, pts, duration, out_flags, output_number, ret_stream_number);

    if (!stream_number && !output_number && !ret_stream_number)
        return E_INVALIDARG;

    ReleaseSemaphore(reader->read_sem, 1, NULL);
    EnterCriticalSection(&reader->cs);

    if (!stream_number)
        stream = NULL;
    else if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
        hr = E_INVALIDARG;
    else if (stream->selection == WMT_OFF)
        hr = NS_E_INVALID_REQUEST;
    else if (stream->eos)
        hr = NS_E_NO_MORE_SAMPLES;

    while (SUCCEEDED(hr))
    {
        struct wg_parser_buffer wg_buffer;
        struct media_buffer *media_buf;
        DWORD flags;

        last_read_stream = stream;

        /* First, see if we can get anything out of the decoder */
        if (stream && !stream->read_compressed)
        {
            hr = media_object_read_all(stream, &buf, pts, duration);
            if (hr != S_FALSE) break;
        }

        if (!stream)
        {
            for (i = 0; i < reader->stream_count; i++)
            {
                last_read_stream = reader->streams + i;
                if (last_read_stream->read_compressed) continue;
                hr = media_object_read_all(last_read_stream, &buf, pts, duration);
                if (hr != S_FALSE) break;
            }
            if (hr != S_FALSE) break;
        }

        /* Got nothing from the decoders, get a buffer from parser and feed it. */
        last_read_stream = stream;
        if (!wg_parser_stream_get_buffer(reader->wg_parser, stream ? stream->wg_stream : 0, &wg_buffer))
        {
            hr = NS_E_NO_MORE_SAMPLES;
            break;
        }
        if (!last_read_stream)
            last_read_stream = wm_reader_get_stream_by_stream_number(reader, wg_buffer.stream + 1);

        TRACE("Got buffer for '%s' stream %p, size: %u.\n",
                get_major_type_string(last_read_stream->compressed_format.major_type),
                last_read_stream, wg_buffer.size);

        if (last_read_stream->read_compressed)
        {
            /* Reading compressed, we can just pass this data along. */
            BYTE *data;
            DWORD size;
            hr = wm_stream_allocate_sample(last_read_stream, wg_buffer.size, &buf);
            if (FAILED(hr))
            {
                hr = NS_E_NO_MORE_SAMPLES;
                break;
            }

            hr = INSSBuffer_GetBufferAndLength(buf, &data, &size);
            if (FAILED(hr)) break;

            if (!wg_parser_stream_copy_buffer(last_read_stream->wg_stream, data, 0, wg_buffer.size))
            {
                hr = NS_E_NO_MORE_SAMPLES;
                break;
            }

            wg_parser_stream_release_buffer(last_read_stream->wg_stream);

            hr = INSSBuffer_SetLength(buf, wg_buffer.size);
            if (FAILED(hr)) break;

            *pts = wg_buffer.pts;
            *duration = wg_buffer.duration;
            *out_flags = 0;
            if (wg_buffer.discontinuity) *out_flags |= WM_SF_DISCONTINUITY;
            if (!wg_buffer.delta) *out_flags |= WM_SF_CLEANPOINT;
            break;
        }

        /* Otherwise, feed the decoder more data. */
        hr = make_media_buffer(wg_buffer.size, &media_buf);
        if (FAILED(hr)) break;

        if (!wg_parser_stream_copy_buffer(last_read_stream->wg_stream, &media_buf->data[0], 0, wg_buffer.size))
        {
            hr = NS_E_NO_MORE_SAMPLES;
            IMediaBuffer_Release(&media_buf->iface);
            break;
        }

        wg_parser_stream_release_buffer(last_read_stream->wg_stream);

        flags = 0;
        media_buf->size = wg_buffer.size;
        if (wg_buffer.has_duration) flags |= DMO_INPUT_DATA_BUFFERF_TIMELENGTH;
        if (wg_buffer.has_pts) flags |= DMO_INPUT_DATA_BUFFERF_TIME;
        hr = IMediaObject_ProcessInput(last_read_stream->decoder, 0, &media_buf->iface, flags, wg_buffer.pts,
                wg_buffer.duration);
        IMediaBuffer_Release(&media_buf->iface);
    }

    if (hr != S_OK && buf)
        INSSBuffer_Release(buf);
    if (hr == NS_E_NO_MORE_SAMPLES && last_read_stream)
        last_read_stream->eos = true;

    if (hr == S_OK)
    {
        *sample = buf;
        if (ret_stream_number)
            *ret_stream_number = last_read_stream - reader->streams + 1;
        if (output_number)
            *output_number = last_read_stream - reader->streams;
    }
    else if (stream_number && ret_stream_number)
        *ret_stream_number = stream_number;

    LeaveCriticalSection(&reader->cs);
    if (WaitForSingleObject(reader->read_sem, INFINITE) != WAIT_OBJECT_0)
        ERR("Failed to wait for read thread to pause.\n");
    TRACE("Returning %#lx\n", hr);
    return hr;
}

static HRESULT WINAPI reader_GetOutputCount(IWMSyncReader2 *iface, DWORD *count)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, count %p.\n", reader, count);

    EnterCriticalSection(&reader->cs);
    *count = reader->stream_count;
    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetOutputFormat(IWMSyncReader2 *iface,
        DWORD output, DWORD index, IWMOutputMediaProps **props)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    HRESULT hr = S_OK;
    AM_MEDIA_TYPE mt;

    TRACE("reader %p, output %lu, index %lu, props %p.\n", reader, output, index, props);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if ((stream->read_compressed && index > 0)
            || (!stream->read_compressed && index >= stream->output_format_count))
        hr = NS_E_INVALID_OUTPUT_FORMAT;
    else if (stream->read_compressed && !amt_from_wg_format(&mt, &stream->compressed_format, true))
        hr = E_INVALIDARG;
    else if (!stream->read_compressed)
        hr = IMediaObject_GetOutputType(stream->decoder, 0, index, &mt);

    LeaveCriticalSection(&reader->cs);

    if (FAILED(hr)) return hr;
    *props = output_props_create(&mt);
    FreeMediaType(&mt);
    return *props ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI reader_GetOutputFormatCount(IWMSyncReader2 *iface, DWORD output, DWORD *count)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output %lu, count %p.\n", reader, output, count);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if (stream->read_compressed) *count = 1;
    else *count = stream->output_format_count;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetOutputNumberForStream(IWMSyncReader2 *iface,
        WORD stream_number, DWORD *output)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, stream_number %u, output %p.\n", reader, stream_number, output);

    *output = stream_number - 1;
    return S_OK;
}

static HRESULT WINAPI reader_GetOutputProps(IWMSyncReader2 *iface,
        DWORD output, IWMOutputMediaProps **props)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output %lu, props %p.\n", reader, output, props);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    *props = output_props_create(&stream->format);
    LeaveCriticalSection(&reader->cs);
    return *props ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI reader_GetOutputSetting(IWMSyncReader2 *iface, DWORD output_num, const WCHAR *name,
        WMT_ATTR_DATATYPE *type, BYTE *value, WORD *length)
{
    struct wm_reader *This = impl_from_IWMSyncReader2(iface);
    FIXME("(%p)->(%lu %s %p %p %p): stub!\n", This, output_num, debugstr_w(name), type, value, length);
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_GetReadStreamSamples(IWMSyncReader2 *iface, WORD stream_number, BOOL *compressed)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %u, compressed %p.\n", reader, stream_number, compressed);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    *compressed = stream->read_compressed;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetStreamNumberForOutput(IWMSyncReader2 *iface,
        DWORD output, WORD *stream_number)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, output %lu, stream_number %p.\n", reader, output, stream_number);

    *stream_number = output + 1;
    return S_OK;
}

static HRESULT WINAPI reader_GetStreamSelected(IWMSyncReader2 *iface,
        WORD stream_number, WMT_STREAM_SELECTION *selection)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %u, selection %p.\n", reader, stream_number, selection);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    *selection = stream->selection;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_Open(IWMSyncReader2 *iface, const WCHAR *filename)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    LARGE_INTEGER size;
    HANDLE file;
    HRESULT hr;

    TRACE("reader %p, filename %s.\n", reader, debugstr_w(filename));

    if ((file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE)
    {
        ERR("Failed to open %s, error %lu.\n", debugstr_w(filename), GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!GetFileSizeEx(file, &size))
    {
        ERR("Failed to get the size of %s, error %lu.\n", debugstr_w(filename), GetLastError());
        CloseHandle(file);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    EnterCriticalSection(&reader->cs);

    if (reader->wg_parser)
    {
        LeaveCriticalSection(&reader->cs);
        WARN("Stream is already open; returning E_UNEXPECTED.\n");
        CloseHandle(file);
        return E_UNEXPECTED;
    }

    reader->filename = wcsdup(filename);
    reader->file = file;
    reader->file_size = size.QuadPart;

    if (FAILED(hr = init_stream(reader)))
    {
        CloseHandle(reader->file);
        reader->file = NULL;
        free(reader->filename);
        reader->filename = NULL;
    }

    LeaveCriticalSection(&reader->cs);
    return hr;
}

static HRESULT WINAPI reader_OpenStream(IWMSyncReader2 *iface, IStream *stream)
{
    static const ULONG64 canary_size = 0xdeadbeeffeedcafe;
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    STATSTG stat;
    HRESULT hr;

    TRACE("reader %p, stream %p.\n", reader, stream);

    stat.cbSize.QuadPart = canary_size;
    if (FAILED(hr = IStream_Stat(stream, &stat, STATFLAG_NONAME)))
    {
        ERR("Failed to stat stream, hr %#lx.\n", hr);
        return hr;
    }

    if (stat.cbSize.QuadPart == canary_size)
    {
        /* Call of Juarez: Gunslinger implements IStream_Stat as an empty function returning S_OK, leaving
         * the output stat unchanged. Windows doesn't call IStream_Seek(_SEEK_END) and probably validates
         * the size against WMV file headers so the bigger cbSize doesn't change anything.
         * Such streams work as soon as the uninitialized cbSize is big enough which is usually the case
         * (if that is not the case Windows will favour shorter cbSize). */
        static const LARGE_INTEGER zero = { 0 };
        ULARGE_INTEGER pos = { .QuadPart = canary_size };

        if (SUCCEEDED(hr = IStream_Seek(stream, zero, STREAM_SEEK_END, &pos)))
            IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
        stat.cbSize.QuadPart = pos.QuadPart == canary_size ? 0 : pos.QuadPart;
        ERR("IStream_Stat did not fill the stream size, size from _Seek %I64u.\n", stat.cbSize.QuadPart);
    }

    EnterCriticalSection(&reader->cs);

    if (reader->wg_parser)
    {
        LeaveCriticalSection(&reader->cs);
        WARN("Stream is already open; returning E_UNEXPECTED.\n");
        return E_UNEXPECTED;
    }

    IStream_AddRef(reader->source_stream = stream);
    reader->file_size = stat.cbSize.QuadPart;

    if (FAILED(hr = init_stream(reader)))
    {
        IStream_Release(stream);
        reader->source_stream = NULL;
    }

    LeaveCriticalSection(&reader->cs);
    return hr;
}

static HRESULT WINAPI reader_SetOutputProps(IWMSyncReader2 *iface, DWORD output, IWMOutputMediaProps *props_iface)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    DWORD mt_size, alignment;
    HRESULT hr = S_OK;
    AM_MEDIA_TYPE *mt;

    TRACE("reader %p, output %lu, props_iface %p.\n", reader, output, props_iface);

    hr = IWMOutputMediaProps_GetMediaType(props_iface, NULL, &mt_size);
    if (FAILED(hr)) return hr;
    mt = calloc(1, mt_size);
    if (!mt) return E_OUTOFMEMORY;
    hr = IWMOutputMediaProps_GetMediaType(props_iface, (WM_MEDIA_TYPE *)mt, &mt_size);
    if (FAILED(hr)) return hr;

    strmbase_dump_media_type(mt);

    ReleaseSemaphore(reader->read_sem, 1, NULL);
    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        hr = E_INVALIDARG;
        goto out;
    }

    if (!IsEqualGUID(&stream->format.majortype, &mt->majortype))
    {
        /* R.U.S.E sets the type of the wrong stream, apparently by accident. */
        hr = NS_E_INCOMPATIBLE_FORMAT;
        goto out;
    }

    if (stream->read_compressed)
    {
        /* Compressed format can't be changed. */
        goto out;
    }

    if (IsEqualGUID(&stream->format.majortype, &MEDIATYPE_Video))
    {
        /* Reject formats that will resize the video output - decoder might accept them */
        WORD current_width = 0, current_height = 0;
        WORD incoming_width = 0, incoming_height = 0;
        if (get_media_type_extent(&stream->format, &current_width, &current_height) &&
                get_media_type_extent(mt, &incoming_width, &incoming_height) &&
                (incoming_width != current_width || incoming_height != current_height))
        {
            hr = NS_E_INVALID_OUTPUT_FORMAT;
            goto out;
        }
    }

    hr = IMediaObject_Flush(stream->decoder);
    if (FAILED(hr)) WARN("Failed to flush decoder. %#lx\n", hr);
    hr = IMediaObject_SetOutputType(stream->decoder, 0, mt, 0);
    if (FAILED(hr))
    {
        WARN("Failed to set media type on decoder: %#lx\n", hr);
        if (hr == DMO_E_TYPE_NOT_ACCEPTED)
        {
            if (IsEqualGUID(&stream->format.majortype, &MEDIATYPE_Audio))
                hr = NS_E_AUDIO_CODEC_NOT_INSTALLED;
            else if (IsEqualGUID(&stream->format.majortype, &MEDIATYPE_Video))
                hr = NS_E_INVALID_OUTPUT_FORMAT;
        }

        goto out;
    }
    hr = IMediaObject_GetOutputSizeInfo(stream->decoder, 0, &stream->sample_size, &alignment);
    if (FAILED(hr)) WARN("Failed to get output size from DMO %#lx\n", hr);

    FreeMediaType(&stream->format);
    CopyMediaType(&stream->format, mt);

out:
    free(mt);
    LeaveCriticalSection(&reader->cs);
    if (WaitForSingleObject(reader->read_sem, INFINITE) != WAIT_OBJECT_0)
        ERR("Failed to wait for read thread to pause.\n");
    return hr;
}

static HRESULT WINAPI reader_SetOutputSetting(IWMSyncReader2 *iface, DWORD output,
        const WCHAR *name, WMT_ATTR_DATATYPE type, const BYTE *value, WORD size)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);

    TRACE("reader %p, output %lu, name %s, type %#x, value %p, size %u.\n",
            reader, output, debugstr_w(name), type, value, size);

    if (!wcscmp(name, L"VideoSampleDurations"))
    {
        FIXME("Ignoring VideoSampleDurations setting.\n");
        return S_OK;
    }
    if (!wcscmp(name, L"EnableDiscreteOutput"))
    {
        FIXME("Ignoring EnableDiscreteOutput setting.\n");
        return S_OK;
    }
    if (!wcscmp(name, L"SpeakerConfig"))
    {
        FIXME("Ignoring SpeakerConfig setting.\n");
        return S_OK;
    }
    else
    {
        FIXME("Unknown setting %s; returning E_NOTIMPL.\n", debugstr_w(name));
        return E_NOTIMPL;
    }
}

static HRESULT WINAPI reader_SetRange(IWMSyncReader2 *iface, QWORD start, LONGLONG duration)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    WORD i;

    TRACE("reader %p, start %I64u, duration %I64d.\n", reader, start, duration);

    ReleaseSemaphore(reader->read_sem, 1, NULL);
    EnterCriticalSection(&reader->cs);

    reader->start_time = start;

    wg_parser_stream_seek(reader->streams[0].wg_stream, 1.0, start, start + duration,
            AM_SEEKING_AbsolutePositioning, duration ? AM_SEEKING_AbsolutePositioning : AM_SEEKING_NoPositioning);

    for (i = 0; i < reader->stream_count; ++i)
    {
        HRESULT hr;
        reader->streams[i].eos = false;
        hr = IMediaObject_Flush(reader->streams[i].decoder);
        if (FAILED(hr)) WARN("Stream %d: Failed to flush decoder.\n", i);
    }

    LeaveCriticalSection(&reader->cs);
    if (WaitForSingleObject(reader->read_sem, INFINITE) != WAIT_OBJECT_0)
        ERR("Failed to wait for read thread to pause.\n");
    return S_OK;
}

static HRESULT WINAPI reader_SetRangeByFrame(IWMSyncReader2 *iface, WORD stream_num, QWORD frame_num,
        LONGLONG frames)
{
    struct wm_reader *This = impl_from_IWMSyncReader2(iface);
    FIXME("(%p)->(%d %s %s): stub!\n", This, stream_num, wine_dbgstr_longlong(frame_num), wine_dbgstr_longlong(frames));
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_SetReadStreamSamples(IWMSyncReader2 *iface, WORD stream_number, BOOL compressed)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    HRESULT hr;

    TRACE("reader %p, stream_index %u, compressed %d.\n", reader, stream_number, compressed);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    hr = IMediaObject_Flush(stream->decoder);
    if (FAILED(hr)) WARN("Failed to flush decoder.\n");
    stream->read_compressed = compressed;

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_SetStreamsSelected(IWMSyncReader2 *iface,
        WORD count, WORD *stream_numbers, WMT_STREAM_SELECTION *selections)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;
    HRESULT hr;
    WORD i;

    TRACE("reader %p, count %u, stream_numbers %p, selections %p.\n",
            reader, count, stream_numbers, selections);

    if (!count)
        return E_INVALIDARG;

    ReleaseSemaphore(reader->read_sem, 1, NULL);
    EnterCriticalSection(&reader->cs);

    for (i = 0; i < count; ++i)
    {
        if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_numbers[i])))
        {
            LeaveCriticalSection(&reader->cs);
            WARN("Invalid stream number %u; returning NS_E_INVALID_REQUEST.\n", stream_numbers[i]);
            if (WaitForSingleObject(reader->read_sem, INFINITE) != WAIT_OBJECT_0)
                ERR("Failed to wait for read thread to pause.\n");
            return NS_E_INVALID_REQUEST;
        }
    }

    for (i = 0; i < count; ++i)
    {
        stream = wm_reader_get_stream_by_stream_number(reader, stream_numbers[i]);
        stream->selection = selections[i];
        if (selections[i] == WMT_OFF)
        {
            TRACE("Disabling stream %u.\n", stream_numbers[i]);
            wg_parser_stream_disable(stream->wg_stream);
            hr = IMediaObject_Flush(stream->decoder);
            if (FAILED(hr)) WARN("Failed to flush decoder.\n");
        }
        else
        {
            if (selections[i] != WMT_ON)
                FIXME("Ignoring selection %#x for stream %u; treating as enabled.\n",
                        selections[i], stream_numbers[i]);
            TRACE("Enabling stream %u.\n", stream_numbers[i]);
            wg_parser_stream_enable(stream->wg_stream, &stream->compressed_format);
        }
    }

    LeaveCriticalSection(&reader->cs);
    if (WaitForSingleObject(reader->read_sem, INFINITE) != WAIT_OBJECT_0)
        ERR("Failed to wait for read thread to pause.\n");
    return S_OK;
}

static HRESULT WINAPI reader_SetRangeByTimecode(IWMSyncReader2 *iface, WORD stream_num,
        WMT_TIMECODE_EXTENSION_DATA *start, WMT_TIMECODE_EXTENSION_DATA *end)
{
    struct wm_reader *This = impl_from_IWMSyncReader2(iface);
    FIXME("(%p)->(%u %p %p): stub!\n", This, stream_num, start, end);
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_SetRangeByFrameEx(IWMSyncReader2 *iface, WORD stream_num, QWORD frame_num,
        LONGLONG frames_to_read, QWORD *starttime)
{
    struct wm_reader *This = impl_from_IWMSyncReader2(iface);
    FIXME("(%p)->(%u %s %s %p): stub!\n", This, stream_num, wine_dbgstr_longlong(frame_num),
          wine_dbgstr_longlong(frames_to_read), starttime);
    return E_NOTIMPL;
}

static HRESULT WINAPI reader_SetAllocateForOutput(IWMSyncReader2 *iface, DWORD output, IWMReaderAllocatorEx *allocator)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output %lu, allocator %p.\n", reader, output, allocator);

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if (stream->output_allocator)
        IWMReaderAllocatorEx_Release(stream->output_allocator);
    if ((stream->output_allocator = allocator))
        IWMReaderAllocatorEx_AddRef(stream->output_allocator);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetAllocateForOutput(IWMSyncReader2 *iface, DWORD output, IWMReaderAllocatorEx **allocator)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, output %lu, allocator %p.\n", reader, output, allocator);

    if (!allocator)
        return E_INVALIDARG;

    EnterCriticalSection(&reader->cs);

    if (!(stream = get_stream_by_output_number(reader, output)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    stream = reader->streams + output;
    if ((*allocator = stream->output_allocator))
        IWMReaderAllocatorEx_AddRef(*allocator);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_SetAllocateForStream(IWMSyncReader2 *iface, DWORD stream_number, IWMReaderAllocatorEx *allocator)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %lu, allocator %p.\n", reader, stream_number, allocator);

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if (stream->stream_allocator)
        IWMReaderAllocatorEx_Release(stream->stream_allocator);
    if ((stream->stream_allocator = allocator))
        IWMReaderAllocatorEx_AddRef(stream->stream_allocator);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static HRESULT WINAPI reader_GetAllocateForStream(IWMSyncReader2 *iface, DWORD stream_number, IWMReaderAllocatorEx **allocator)
{
    struct wm_reader *reader = impl_from_IWMSyncReader2(iface);
    struct wm_stream *stream;

    TRACE("reader %p, stream_number %lu, allocator %p.\n", reader, stream_number, allocator);

    if (!allocator)
        return E_INVALIDARG;

    EnterCriticalSection(&reader->cs);

    if (!(stream = wm_reader_get_stream_by_stream_number(reader, stream_number)))
    {
        LeaveCriticalSection(&reader->cs);
        return E_INVALIDARG;
    }

    if ((*allocator = stream->stream_allocator))
        IWMReaderAllocatorEx_AddRef(*allocator);

    LeaveCriticalSection(&reader->cs);
    return S_OK;
}

static const IWMSyncReader2Vtbl reader_vtbl =
{
    reader_QueryInterface,
    reader_AddRef,
    reader_Release,
    reader_Open,
    reader_Close,
    reader_SetRange,
    reader_SetRangeByFrame,
    reader_GetNextSample,
    reader_SetStreamsSelected,
    reader_GetStreamSelected,
    reader_SetReadStreamSamples,
    reader_GetReadStreamSamples,
    reader_GetOutputSetting,
    reader_SetOutputSetting,
    reader_GetOutputCount,
    reader_GetOutputProps,
    reader_SetOutputProps,
    reader_GetOutputFormatCount,
    reader_GetOutputFormat,
    reader_GetOutputNumberForStream,
    reader_GetStreamNumberForOutput,
    reader_GetMaxOutputSampleSize,
    reader_GetMaxStreamSampleSize,
    reader_OpenStream,
    reader_SetRangeByTimecode,
    reader_SetRangeByFrameEx,
    reader_SetAllocateForOutput,
    reader_GetAllocateForOutput,
    reader_SetAllocateForStream,
    reader_GetAllocateForStream
};

HRESULT WINAPI winegstreamer_create_wm_sync_reader(IUnknown *outer, void **out)
{
    struct wm_reader *object;

    TRACE("out %p.\n", out);

    if (!init_gstreamer())
        return E_FAIL;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IUnknown_inner.lpVtbl = &unknown_inner_vtbl;
    object->IWMSyncReader2_iface.lpVtbl = &reader_vtbl;
    object->IWMHeaderInfo3_iface.lpVtbl = &header_info_vtbl;
    object->IWMLanguageList_iface.lpVtbl = &language_list_vtbl;
    object->IWMPacketSize2_iface.lpVtbl = &packet_size_vtbl;
    object->IWMProfile3_iface.lpVtbl = &profile_vtbl;
    object->IWMReaderPlaylistBurn_iface.lpVtbl = &playlist_vtbl;
    object->IWMReaderTimecode_iface.lpVtbl = &timecode_vtbl;
    object->outer = outer ? outer : &object->IUnknown_inner;
    object->refcount = 1;

    InitializeCriticalSectionEx(&object->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": reader.cs");
    InitializeCriticalSectionEx(&object->shutdown_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->shutdown_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": reader.shutdown_cs");

    TRACE("Created reader %p.\n", object);
    *out = outer ? (void *)&object->IUnknown_inner : (void *)&object->IWMSyncReader2_iface;
    return S_OK;
}
