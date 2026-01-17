/*
 * ntoskrnl.exe implementation
 *
 * Copyright (C) 2007 Alexandre Julliard
 * Copyright (C) 2010 Damjan Jovanovic
 * Copyright (C) 2016 Sebastian Lackner
 * Copyright (C) 2016 CodeWeavers, Aric Stewart
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

/*************************************************************************
 *           RtlUshortByteSwap   (NTOSKRNL.EXE.@)
 */
__ASM_FASTCALL_FUNC(RtlUshortByteSwap, 4,
                    "movb %ch,%al\n\t"
                    "movb %cl,%ah\n\t"
                    "ret")

/*************************************************************************
 *           RtlUlongByteSwap   (NTOSKRNL.EXE.@)
 */
__ASM_FASTCALL_FUNC(RtlUlongByteSwap, 4,
                    "movl %ecx,%eax\n\t"
                    "bswap %eax\n\t"
                    "ret")

/*************************************************************************
 *           RtlUlonglongByteSwap   (NTOSKRNL.EXE.@)
 */
__ASM_FASTCALL_FUNC(RtlUlonglongByteSwap, 8,
                    "movl 4(%esp),%edx\n\t"
                    "bswap %edx\n\t"
                    "movl 8(%esp),%eax\n\t"
                    "bswap %eax\n\t"
                    "ret $8")

#endif  /* __i386__ */
