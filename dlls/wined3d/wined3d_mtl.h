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

#ifndef __WINE_WINED3D_MTL_H
#define __WINE_WINED3D_MTL_H

#include "wine/wined3d.h"
#include "wined3d_private.h"

struct wined3d_context_mtl
{
    struct wined3d_context c;
};

static inline struct wined3d_context_mtl *wined3d_context_mtl(struct wined3d_context *context)
{
    return CONTAINING_RECORD(context, struct wined3d_context_mtl, c);
}

struct wined3d_adapter_mtl
{
    struct wined3d_adapter a;
};

static inline struct wined3d_adapter_mtl *wined3d_adapter_mtl(struct wined3d_adapter *adapter)
{
    return CONTAINING_RECORD(adapter, struct wined3d_adapter_mtl, a);
}

struct wined3d_query_mtl
{
    struct wined3d_query q;
};

static inline struct wined3d_query_mtl *wined3d_query_mtl(struct wined3d_query *query)
{
    return CONTAINING_RECORD(query, struct wined3d_query_mtl, q);
}

struct wined3d_texture_mtl
{
    struct wined3d_texture t;
};

static inline struct wined3d_texture_mtl *wined3d_texture_mtl(struct wined3d_texture *texture)
{
    return CONTAINING_RECORD(texture, struct wined3d_texture_mtl, s);
}

HRESULT wined3d_texture_mtl_init(struct wined3d_texture_mtl *texture_mtl, struct wined3d_device *device,
                                 const struct wined3d_resource_desc *desc, unsigned int layer_count, unsigned int level_count,
                                 uint32_t flags, void *parent, const struct wined3d_parent_ops *parent_ops);

struct wined3d_sampler_mtl
{
    struct wined3d_sampler s;
};

static inline struct wined3d_sampler_mtl *wined3d_sampler_mtl(struct wined3d_sampler *sampler)
{
    return CONTAINING_RECORD(sampler, struct wined3d_sampler_mtl, s);
}

void wined3d_sampler_mtl_init(struct wined3d_sampler_mtl *sampler_mtl,
                              struct wined3d_device *device, const struct wined3d_sampler_desc *desc,
                              void *parent, const struct wined3d_parent_ops *parent_ops);

struct wined3d_buffer_mtl
{
    struct wined3d_buffer b;
};

static inline struct wined3d_buffer_mtl *wined3d_buffer_mtl(struct wined3d_buffer *buffer)
{
    return CONTAINING_RECORD(buffer, struct wined3d_buffer_mtl, b);
}

HRESULT wined3d_buffer_mtl_init(struct wined3d_buffer_mtl *buffer_mtl, struct wined3d_device *device,
        const struct wined3d_buffer_desc *desc, const struct wined3d_sub_resource_data *data,
        void *parent, const struct wined3d_parent_ops *parent_ops);

#endif /* __WINE_WINED3D_MTL_H */
