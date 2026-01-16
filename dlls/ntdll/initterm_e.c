/*
 * _initterm_e fallback implementation
 *
 * Copyright 2025 Yuxuan Shui for CodeWeavers
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

#include <process.h>
#include "minwindef.h"

#include "wine/asm.h"

#if 0
#pragma makedep implib
#endif

/* This is a fallback version of msvcrt's _initterm_e. Since _initterm_e is used in winecrt0,
 * if the final object is linked with a version of msvcrt without the _initterm_e function, this
 * version will be used. */
int CDECL _initterm_e( _PIFV *table, _PIFV *end )
{
    int res = 0;

    while (!res && table < end)
    {
        if (*table) res = (**table)();
        table++;
    }
    return res;
}

__ASM_GLOBAL_IMPORT(_initterm_e)
