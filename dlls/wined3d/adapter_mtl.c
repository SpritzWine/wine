/*
 * Copyright 2024 Isaac Marovitz
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

#include "wined3d_private.h"
#include "wined3d_mtl.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

static void adapter_mtl_get_wined3d_caps(const struct wined3d_adapter *adapter, struct wined3d_caps *caps)
{
    caps->ddraw_caps.dds_caps |= WINEDDSCAPS_BACKBUFFER
            | WINEDDSCAPS_COMPLEX
            | WINEDDSCAPS_FRONTBUFFER
            | WINEDDSCAPS_3DDEVICE
            | WINEDDSCAPS_VIDEOMEMORY
            | WINEDDSCAPS_OWNDC
            | WINEDDSCAPS_LOCALVIDMEM
            | WINEDDSCAPS_NONLOCALVIDMEM;

    caps->ddraw_caps.caps |= WINEDDCAPS_3D;

    caps->Caps2 |= WINED3DCAPS2_CANGENMIPMAP;

    caps->PrimitiveMiscCaps |= WINED3DPMISCCAPS_BLENDOP
            | WINED3DPMISCCAPS_INDEPENDENTWRITEMASKS
            | WINED3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS
            | WINED3DPMISCCAPS_POSTBLENDSRGBCONVERT
            | WINED3DPMISCCAPS_SEPARATEALPHABLEND;

    caps->RasterCaps |= WINED3DPRASTERCAPS_MIPMAPLODBIAS
            | WINED3DPRASTERCAPS_ANISOTROPY;

    caps->TextureFilterCaps |= WINED3DPTFILTERCAPS_MAGFANISOTROPIC
            | WINED3DPTFILTERCAPS_MINFANISOTROPIC;

    caps->MaxAnisotropy = 16;

    caps->SrcBlendCaps |= WINED3DPBLENDCAPS_BLENDFACTOR;

    caps->DestBlendCaps |= WINED3DPBLENDCAPS_BLENDFACTOR
            | WINED3DPBLENDCAPS_SRCALPHASAT;

    caps->TextureCaps |= WINED3DPTEXTURECAPS_VOLUMEMAP
            | WINED3DPTEXTURECAPS_MIPVOLUMEMAP
            | WINED3DPTEXTURECAPS_VOLUMEMAP_POW2;

    caps->VolumeTextureFilterCaps |= WINED3DPTFILTERCAPS_MAGFLINEAR
            | WINED3DPTFILTERCAPS_MAGFPOINT
            | WINED3DPTFILTERCAPS_MINFLINEAR
            | WINED3DPTFILTERCAPS_MINFPOINT
            | WINED3DPTFILTERCAPS_MIPFLINEAR
            | WINED3DPTFILTERCAPS_MIPFPOINT
            | WINED3DPTFILTERCAPS_LINEAR
            | WINED3DPTFILTERCAPS_LINEARMIPLINEAR
            | WINED3DPTFILTERCAPS_LINEARMIPNEAREST
            | WINED3DPTFILTERCAPS_MIPLINEAR
            | WINED3DPTFILTERCAPS_MIPNEAREST
            | WINED3DPTFILTERCAPS_NEAREST;

    caps->VolumeTextureAddressCaps |= WINED3DPTADDRESSCAPS_INDEPENDENTUV
            | WINED3DPTADDRESSCAPS_CLAMP
            | WINED3DPTADDRESSCAPS_WRAP;
            | WINED3DPTADDRESSCAPS_BORDER
            | WINED3DPTADDRESSCAPS_MIRROR
            | WINED3DPTADDRESSCAPS_MIRRORONCE;

    // TODO:
    // caps->MaxVolumeExtent = ;

    caps->CubeTextureFilterCaps |= WINED3DPTFILTERCAPS_MAGFLINEAR
            | WINED3DPTFILTERCAPS_MAGFPOINT
            | WINED3DPTFILTERCAPS_MINFLINEAR
            | WINED3DPTFILTERCAPS_MINFPOINT
            | WINED3DPTFILTERCAPS_MIPFLINEAR
            | WINED3DPTFILTERCAPS_MIPFPOINT
            | WINED3DPTFILTERCAPS_LINEAR
            | WINED3DPTFILTERCAPS_LINEARMIPLINEAR
            | WINED3DPTFILTERCAPS_LINEARMIPNEAREST
            | WINED3DPTFILTERCAPS_MIPLINEAR
            | WINED3DPTFILTERCAPS_MIPNEAREST
            | WINED3DPTFILTERCAPS_NEAREST
            | WINED3DPTFILTERCAPS_MAGFANISOTROPIC
            | WINED3DPTFILTERCAPS_MINFANISOTROPIC;

    caps->TextureAddressCaps |= WINED3DPTADDRESSCAPS_BORDER
            | WINED3DPTADDRESSCAPS_MIRROR
            | WINED3DPTADDRESSCAPS_MIRRORONCE;

    caps->StencilCaps |= WINED3DSTENCILCAPS_DECR
            | WINED3DSTENCILCAPS_INCR
            | WINED3DSTENCILCAPS_TWOSIDED;

    caps->DeclTypes |= WINED3DDTCAPS_FLOAT16_2 | WINED3DDTCAPS_FLOAT16_4;

    caps->MaxPixelShader30InstructionSlots = WINED3DMAX30SHADERINSTRUCTIONS;
    caps->MaxVertexShader30InstructionSlots = WINED3DMAX30SHADERINSTRUCTIONS;
    caps->PS20Caps.temp_count = WINED3DPS20_MAX_NUMTEMPS;
    caps->VS20Caps.temp_count = WINED3DVS20_MAX_NUMTEMPS;
}

static BOOL adapter_mtl_check_format(const struct wined3d_adapter *adapter,
        const struct wined3d_format *adapter_format, const struct wined3d_format *rt_format,
        const struct wined3d_format *ds_format)
{
    return TRUE;
}

static HRESULT adapter_mtl_create_buffer(struct wined3d_device *device,
        const struct wined3d_buffer_desc *desc, const struct wined3d_sub_resource_data *data,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_buffer **buffer)
{
    struct wined3d_buffer_mtl *buffer_mtl;
    HRESULT hr;

    TRACE("device %p, desc %p, data %p, parent %p, parent_ops %p, buffer %p.\n",
          device, desc, data, parent, parent_ops, buffer);

    if (!(buffer_mtl = calloc(1, sizeof(*buffer_mtl))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_buffer_mtl_init(buffer_mtl, device, desc, data, parent, parent_ops)))
    {
        WARN("Failed to initialise buffer, hr %#lx.\n", hr);
        free(buffer_mtl);
        return hr;
    }

    TRACE("Created buffer %p.\n", buffer_mtl);
    *buffer = &buffer_mtl->b;

    return hr;
}

static void adapter_mtl_destroy_buffer(struct wined3d_buffer *buffer)
{
    struct wined3d_buffer_mtl *buffer_mtl = wined3d_buffer_mtl(buffer);
    struct wined3d_device *device = buffer_mtl->b.resource.device;
    unsigned int swapchain_count = device->swapchain_count;

    TRACE("buffer_mtl %p.\n", buffer_mtl);

    /* Take a reference to the device, in case releasing the buffer would
    * cause the device to be destroyed. However, swapchain resources don't
    * take a reference to the device, and we wouldn't want to increment the
    * refcount on a device that's in the process of being destroyed. */
    if (swapchain_count)
        wined3d_device_incref(device);
    wined3d_buffer_cleanup(&buffer_mtl->b);
    wined3d_cs_destroy_object(device->cs, free, buffer_mtl);
    if (swapchain_count)
        wined3d_device_decref(device);
}

static HRESULT adapter_mtl_create_texture(struct wined3d_device *device,
        const struct wined3d_resource_desc *desc, unsigned int layer_count, unsigned int level_count,
        uint32_t flags, void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_texture **texture)
{
    struct wined3d_texture_mtl *texture_mtl;
    HRESULT hr;

    TRACE("device %p, desc %p, layer_count %u, level_count %u, flags %#x, parent %p, parent_ops %p, texture %p.\n",
          device, desc, layer_count, level_count, flags, parent, parent_ops, texture);

    if (!(texture_mtl = wined3d_texture_allocate_object_memory(sizeof(*texture_mtl), level_count, layer_count)))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_texture_mtl_init(texture_mtl, device, desc,
                                            layer_count, level_count, flags, parent, parent_ops)))
    {
        WARN("Failed to initialise texture, hr %#lx.\n", hr);
        free(texture_mtl);
        return hr;
    }

    TRACE("Created texture %p.\n", texture_mtl);
    *texture = &texture_mtl->t;

    return hr;
}

static void adapter_mtl_destroy_texture(struct wined3d_texture *texture)
{
    struct wined3d_texture_mtl *texture_mtl = wined3d_texture_mtl(texture);
    struct wined3d_device *device = texture_mtl->t.resource.device;
    unsigned int swapchain_count = device->swapchain_count;

    TRACE("texture_mtl %p.\n", texture_mtl);

    /* Take a reference to the device, in case releasing the texture would
     * cause the device to be destroyed. However, swapchain resources don't
     * take a reference to the device, and we wouldn't want to increment the
     * refcount on a device that's in the process of being destroyed. */
    if (swapchain_count)
        wined3d_device_incref(device);

    wined3d_texture_sub_resources_destroyed(texture);
    texture->resource.parent_ops->wined3d_object_destroyed(texture->resource.parent);

    wined3d_texture_cleanup(&texture_mtl->t);
    wined3d_cs_destroy_object(device->cs, free, texture_mtl);

    if (swapchain_count)
        wined3d_device_decref(device);
}

static HRESULT adapter_mtl_create_sampler(struct wined3d_device *device, const struct wined3d_sampler_desc *desc,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_sampler **sampler)
{
    struct wined3d_sampler_mtl *sampler_mtl;

    TRACE("device %p, desc %p, parent %p, parent_ops %p, sampler %p.\n",
          device, desc, parent, parent_ops, sampler);

    if (!(sampler_mtl = calloc(1, sizeof(*sampler_mtl))))
        return E_OUTOFMEMORY;

    wined3d_sampler_mtl_init(sampler_mtl, device, desc, parent, parent_ops);

    TRACE("Created sampler %p.\n", sampler_mtl);
    *sampler = &sampler_mtl->s;

    return WINED3D_OK;
}

static void wined3d_sampler_mtl_destroy_object(void *object)
{
    struct wined3d_sampler_mtl *sampler_mtl = object;

    TRACE("sampler_mtl %p.\n", sampler_mtl);

    free(sampler_mtl);
}

static void adapter_mtl_destroy_sampler(struct wined3d_sampler *sampler) {
    struct wined3d_sampler_mtl *sampler_mtl = wined3d_sampler_mtl(sampler);

    TRACE("sampler_mtl %p.\n", sampler_mtl);

    wined3d_cs_destroy_object(sampler->device->cs, wined3d_sampler_mtl_destroy_object, sampler_mtl);
}

static HRESULT adapter_mtl_create_query(struct wined3d_device *device, enum wined3d_query_type type,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_query **query)
{
    TRACE("device %p, type %#x, parent %p, parent_ops %p, query %p.\n",
          device, type, parent, parent_ops, query);

    // TODO: Fix
    return WINED3DERR_NOTAVAILABLE;
}

static void wined3d_query_mtl_destroy_object(void *object)
{
    struct wined3d_query_mtl *query_mtl = object;

    TRACE("query_mtl %p.\n", query_mtl);

    query_mtl->q.query_ops->query_destroy(&query_mtl->q);
}

static void adapter_mtl_destroy_query(struct wined3d_query *query)
{
    struct wined3d_query_mtl *query_mtl = wined3d_query_mtl(query);

    TRACE("query_mtl %p.\n", query_mtl);

    wined3d_cs_destroy_object(query->device->cs, wined3d_query_mtl_destroy_object, query_mtl);
}

static void adapter_mtl_flush_context(struct wined3d_context *context)
{
    struct wined3d_context_mtl *context_mtl = wined3d_context_mtl(context);

    TRACE("context_mtl %p.\n", context_mtl);

    // TODO: Fix
    // wined3d_context_mtl_submit_command_buffer(context_mtl);
}

static void adapter_mtl_draw_primitive(struct wined3d_device *device,
        const struct wined3d_state *state, const struct wined3d_draw_parameters *parameters)
{
    struct wined3d_buffer_mtl *indirect_mtl = NULL;
    // const struct wined3d_mtl_info *mtl_info;
    struct wined3d_context_mtl *context_mtl;
    // CMDBuff
    uint32_t instance_count;

    TRACE("device %p, state %p, parameters %p.\n", device, state, parameters);

    context_mtl = wined3d_context_mtl(context_acquire(device, NULL, 0));

    if (parameters->indirect)
        indirect_mtl = wined3d_buffer_mtl(parameters->u.indirect.buffer);

    if (context_mtl->c.transform_feedback_active)
    {
        WARN("Transform feedback is not supported in Metal.");
    }

    if (parameters->indirect)
    {
        if (parameters->indexed)

        else
    }
    else
    {
        instance_count = parameters->u.direct.instance_count;
        if (!instance_count)
            instance_count = 1;

        if (parameters->indexed)

        else
    }

    context_release(&context_mtl->c);
}

static void adapter_mtl_dispatch_compute(struct wined3d_device *device,
        const struct wined3d_state *state, const struct wined3d_dispatch_parameters *parameters)
{
    struct wined3d_buffer_mtl *indirect_mtl = NULL;
    // const struct wined3d_mtl_info *mtl_info;
    struct wined3d_context_mtl *context_mtl;
    // CMDBuff

    TRACE("device %p, state %p, parameters %p.\n", device, state, parameters);

    context_mtl = wined3d_context_mtl(context_acquire(device, NULL, 0));

    if (parameters->indirect)
        indirect_mtl = wined3d_buffer_mtl(parameters->u.indirect.buffer);

    if (parameters->indirect)
    {
        // TODO
    }
    else
    {
        const struct wined3d_direct_dispatch_parameters *direct = &parameters->u.direct;
    }

    context_release(&context_mtl->c);
}

static const struct wined3d_adapter_ops wined3d_adapter_mtl_ops =
{
    .adapter_destroy = NULL,
    .adapter_create_device = NULL,
    .adapter_destroy_device = NULL,
    .adapter_acquire_context = NULL,
    .adapter_release_context = NULL,
    .adapter_get_wined3d_caps = adapter_mtl_get_wined3d_caps,
    .adapter_check_format = adapter_mtl_check_format,
    .adapter_init_3d = NULL,
    .adapter_uninit_3d = NULL,
    .adapter_map_bo_address = NULL,
    .adapter_unmap_bo_address = NULL,
    .adapter_copy_bo_address = NULL,
    .adapter_flush_bo_address = NULL,
    .adapter_alloc_bo = NULL,
    .adapter_destroy_bo = NULL,
    .adapter_create_swapchain = NULL,
    .adapter_destroy_swapchain = NULL,
    .adapter_create_buffer = adapter_mtl_create_buffer,
    .adapter_destroy_buffer = adapter_mtl_destroy_buffer,
    .adapter_create_texture = adapter_mtl_create_texture,
    .adapter_destroy_texture = adapter_mtl_destroy_texture,
    .adapter_create_rendertarget_view = NULL,
    .adapter_destroy_rendertarget_view = NULL,
    .adapter_create_shader_resource_view = NULL,
    .adapter_destroy_shader_resource_view = NULL,
    .adapter_create_unordered_access_view = NULL,
    .adapter_destroy_unordered_access_view = NULL,
    .adapter_create_sampler = adapter_mtl_create_sampler,
    .adapter_destroy_sampler = adapter_mtl_destroy_sampler,
    .adapter_create_query = adapter_mtl_create_query,
    .adapter_destroy_query = adapter_mtl_destroy_query,
    .adapter_flush_context = adapter_mtl_flush_context,
    .adapter_draw_primitive = adapter_mtl_draw_primitive,
    .adapter_dispatch_compute = adapter_mtl_dispatch_compute,
    .adapter_clear_uav = NULL,
    .adapter_generate_mipmap = NULL,
};

static BOOL wined3d_adapter_mtl_init(struct wined3d_adapter_mtl *adapter_mtl,
        unsigned int ordinal, unsigned int wined3d_creation_flags)
{
    TRACE("adapter_mtl %p, ordinal %u, wined3d_creation_flags %#x.\n",
          adapter_mtl, ordinal, wined3d_creation_flags);

    return FALSE;
}

struct wined3d_adapter *wined3d_adapter_mtl_create(unsigned int ordinal,
        unsigned int wined3d_creation_flags)
{
    struct wined3d_adapter_mtl *adapter_mtl;

    if (!(adapter_mtl = calloc(1, sizeof(*adapter_mtl))))
        return NULL;

    if (!wined3d_adapter_mtl_init(adapter_mtl, ordinal, wined3d_creation_flags))
    {
        free(adapter_mtl);
        return NULL;
    }

    TRACE("Created adapter %p.\n", adapter_mtl);

    return &adapter_mtl->a;
}