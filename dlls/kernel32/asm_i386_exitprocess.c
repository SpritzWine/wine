/*
 * Win32 processes
 *
 * Copyright 1996, 1998 Alexandre Julliard
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

#include "wine/asm.h"


#ifdef __i386__

__ASM_STDCALL_FUNC( ExitProcess, 4, /* Shrinker depend on this particular ExitProcess implementation */
                   "pushl %ebp\n\t"
                   ".byte 0x8B, 0xEC\n\t" /* movl %esp, %ebp */
                   ".byte 0x6A, 0x00\n\t" /* pushl $0 */
                   ".byte 0x68, 0x00, 0x00, 0x00, 0x00\n\t" /* pushl $0 - 4 bytes immediate */
                   "pushl 8(%ebp)\n\t"
                   "call " __ASM_STDCALL("RtlExitUserProcess",4) "\n\t"
                   "leave\n\t"
                   "ret $4" )

#endif /* __i386__ */
