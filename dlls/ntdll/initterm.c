/*
 * _initterm fallback implementation
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

/* This is a fallback version of msvcrt's _initterm. Since _initterm is used in winecrt0,
 * if the final object is linked with a version of msvcrt without the _initterm function, this
 * version will be used. */
void CDECL _initterm( _PVFV *start,_PVFV *end )
{
    _PVFV *current = start;

    while (current < end)
    {
        if (*current) (**current)();
        current++;
    }
}

__ASM_GLOBAL_IMPORT(_initterm)
