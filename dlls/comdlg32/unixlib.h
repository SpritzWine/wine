/*
 * comdlg32 Unix library interface.
 *
 * Bridges Wine's classic file open/save dialogs to the host's native file
 * chooser via the XDG desktop portal (org.freedesktop.portal.FileChooser).
 * The PE side (portal.c) marshals a request and makes a __wine_unix_call into
 * comdlg32.so, whose dispatch table (unixlib.c) forwards to the Rust portal
 * client in libwine_comdlg32.so.
 *
 * Copyright 2026 The SpritzWine project
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

#ifndef __WINE_COMDLG32_UNIXLIB_H
#define __WINE_COMDLG32_UNIXLIB_H

#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

/* Request/response for unix_file_chooser. All strings are UTF-8. This is a
 * 32-bit-only project, so the PE and Unix sides share one address space and no
 * wow64 pointer thunking is required. */
struct file_chooser_params
{
    BOOL        save;          /* TRUE: Save dialog, FALSE: Open dialog */
    const char *title;         /* dialog title, or NULL */
    const char *initial_dir;   /* starting host directory, or NULL */
    const char *current_name;  /* suggested name for Save, or NULL */
    const char *filters;       /* "label\0glob;glob\0...\0\0", or NULL */
    char       *out_path;      /* out: chosen host path, NUL-terminated UTF-8 */
    ULONG       out_size;      /* in: capacity of out_path (bytes);
                                * out: bytes required including the NUL */
};

enum comdlg32_funcs
{
    unix_file_chooser,
    unix_funcs_count,
};

#define COMDLG32_CALL( func, params ) WINE_UNIX_CALL( unix_ ## func, (params) )

#endif /* __WINE_COMDLG32_UNIXLIB_H */
