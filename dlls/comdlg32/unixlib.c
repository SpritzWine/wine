/*
 * comdlg32 Unix library: __wine_unix_call dispatch table.
 *
 * The portal / D-Bus client itself lives in the Rust crate wine_comdlg32
 * (libwine_comdlg32.so), linked in through UNIX_LIBS. This file only wires the
 * unix-call entry points to it.
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "unixlib.h"

/* Implemented in Rust: rust/src_dll/comdlg32 -> libwine_comdlg32.so. */
extern NTSTATUS rs_portal_file_chooser( void *args );
extern NTSTATUS rs_portal_prepare_print( void *args );

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    rs_portal_file_chooser,   /* unix_file_chooser */
    rs_portal_prepare_print,  /* unix_prepare_print */
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

/* WoW64 thunk: a 32-bit PE app passes file_chooser_params with 32-bit pointers,
 * but this unix lib is 64-bit. Widen the pointers into the native struct and
 * call the same Rust entry; the strings and the output buffer live in the shared
 * low address space, so only the scalar out fields need copying back. */
typedef ULONG PTR32;

struct file_chooser_params32
{
    BOOL  save;
    BOOL  multiple;
    BOOL  directory;
    PTR32 title;
    PTR32 initial_dir;
    PTR32 current_name;
    PTR32 filters;
    ULONG current_filter;
    PTR32 out_paths;
    ULONG out_size;
    ULONG out_filter;
};

static NTSTATUS wow64_file_chooser( void *args )
{
    struct file_chooser_params32 *p32 = args;
    struct file_chooser_params params;
    NTSTATUS status;

    params.save           = p32->save;
    params.multiple       = p32->multiple;
    params.directory      = p32->directory;
    params.title          = ULongToPtr( p32->title );
    params.initial_dir    = ULongToPtr( p32->initial_dir );
    params.current_name   = ULongToPtr( p32->current_name );
    params.filters        = ULongToPtr( p32->filters );
    params.current_filter = p32->current_filter;
    params.out_paths      = ULongToPtr( p32->out_paths );
    params.out_size       = p32->out_size;
    params.out_filter     = 0;

    status = rs_portal_file_chooser( &params );

    p32->out_size   = params.out_size;
    p32->out_filter = params.out_filter;
    return status;
}

/* prepare_print_params is one pointer followed by scalars and fixed arrays, so
 * the 32-bit layout is the native one with the pointer narrowed: widen it and
 * copy the rest through unchanged. */
static NTSTATUS wow64_prepare_print( void *args )
{
    struct prepare_print_params params;
    const size_t rest = FIELD_OFFSET(struct prepare_print_params, output_file)
                        + sizeof(params.output_file)
                        - FIELD_OFFSET(struct prepare_print_params, copies);
    BYTE *rest32 = (BYTE *)args + sizeof(PTR32);
    NTSTATUS status;

    params.title = ULongToPtr( *(PTR32 *)args );
    memcpy( &params.copies, rest32, rest );

    status = rs_portal_prepare_print( &params );

    memcpy( rest32, &params.copies, rest );
    return status;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_file_chooser,   /* unix_file_chooser */
    wow64_prepare_print,  /* unix_prepare_print */
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif /* _WIN64 */
