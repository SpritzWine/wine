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

/* Request/response for unix_file_chooser. All strings are UTF-8. In a same-width
 * build (pure 32- or 64-bit) the PE and Unix sides share one address space; the
 * 32-bit-PE-on-64-bit-unix (wow64) case is handled by a thunk in unixlib.c. */
struct file_chooser_params
{
    BOOL        save;           /* TRUE: Save dialog, FALSE: Open dialog */
    BOOL        multiple;       /* allow selecting multiple files */
    BOOL        directory;      /* pick a directory rather than a file */
    const char *title;          /* dialog title, or NULL */
    const char *initial_dir;    /* starting host directory, or NULL */
    const char *current_name;   /* suggested name for Save, or NULL */
    const char *filters;        /* "label\0glob;glob\0...\0\0", or NULL */
    ULONG       current_filter; /* 1-based index of the filter to pre-select, 0 = none */
    char       *out_paths;      /* out: chosen host path(s), each NUL-terminated,
                                 * the list double-NUL-terminated, UTF-8 */
    ULONG       out_size;       /* in: capacity of out_paths (bytes);
                                 * out: bytes required including the final NUL */
    ULONG       out_filter;     /* out: 1-based index of the filter the user
                                 * selected, 0 = unknown */
};

/* Request/response for unix_prepare_print: the host print dialog
 * (org.freedesktop.portal.Print.PreparePrint). Results are scalars and fixed
 * arrays so the wow64 thunk has a single pointer to widen. Mirrored by
 * PreparePrintParams in rust/src_dll/comdlg32/src/lib.rs. */
struct prepare_print_params
{
    const char *title;          /* dialog title, or NULL */
    ULONG copies;               /* in: hint (0 = none); out: chosen */
    BOOL  collate;              /* out */
    ULONG orientation;          /* in/out: PORTAL_ORIENT_* */
    BOOL  has_current_page;     /* in: "current page" is meaningful for the app */
    BOOL  has_selected_pages;   /* in: "selection" is meaningful for the app */
    BOOL  print_to_file;        /* in: the app allows print-to-file (not every
                                 * backend can hide it) */
    ULONG token;                /* out: lets the later Print call skip the dialog */
    ULONG duplex;               /* out: PORTAL_DUPLEX_* */
    BOOL  color;                /* out */
    ULONG resolution;           /* out: dpi, 0 = unknown */
    BOOL  reverse;              /* out */
    ULONG print_pages;          /* out: PORTAL_PAGES_* */
    ULONG paper_width;          /* out: tenths of a millimetre, 0 = unknown */
    ULONG paper_height;
    char  paper_name[64];       /* out: PPD or display name, UTF-8 */
    char  page_ranges[256];     /* out: the portal's range string, e.g. "1-3,5" */
    char  output_file[1024];    /* out: host path when printing to a file, else "" */
};

#define PORTAL_ORIENT_UNKNOWN   0
#define PORTAL_ORIENT_PORTRAIT  1
#define PORTAL_ORIENT_LANDSCAPE 2

#define PORTAL_DUPLEX_SIMPLEX   0
#define PORTAL_DUPLEX_LONG_EDGE 1
#define PORTAL_DUPLEX_SHORT_EDGE 2

#define PORTAL_PAGES_ALL        0
#define PORTAL_PAGES_RANGES     1
#define PORTAL_PAGES_CURRENT    2
#define PORTAL_PAGES_SELECTION  3

/* Delivery of a spooled document (rs_portal_print in the Rust crate). Not in
 * the dispatch table: localspl.so links the same libwine_comdlg32.so and calls
 * it directly for its PORTAL: port; the struct is repeated in
 * dlls/localspl/cups.c and in lib.rs. */
struct portal_print_params
{
    const char *path;           /* host path of the finished PostScript document */
    const char *title;          /* job title, or NULL */
};

enum comdlg32_funcs
{
    unix_file_chooser,
    unix_prepare_print,
    unix_funcs_count,
};

#define COMDLG32_CALL( func, params ) WINE_UNIX_CALL( unix_ ## func, (params) )

#endif /* __WINE_COMDLG32_UNIXLIB_H */
