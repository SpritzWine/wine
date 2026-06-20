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

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    rs_portal_file_chooser,   /* unix_file_chooser */
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
