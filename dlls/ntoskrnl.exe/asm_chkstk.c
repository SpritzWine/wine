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


/**************************************************************************
 *		__chkstk (NTOSKRNL.@)
 */
#ifdef __x86_64__

/* Supposed to touch all the stack pages, but we shouldn't need that. */
__ASM_GLOBAL_FUNC( __chkstk, "ret" );

#elif defined(__i386__)

__ASM_GLOBAL_FUNC( _chkstk,
                   "negl %eax\n\t"
                   "addl %esp,%eax\n\t"
                   "xchgl %esp,%eax\n\t"
                   "movl 0(%eax),%eax\n\t"  /* copy return address from old location */
                   "movl %eax,0(%esp)\n\t"
                   "ret" )

#elif defined(__arm__)

/* Incoming r4 contains words to allocate, converting to bytes then return */
__ASM_GLOBAL_FUNC( __chkstk, "lsl r4, r4, #2\n\t"
                             "bx lr" )

#elif defined(__aarch64__)

/* Supposed to touch all the stack pages, but we shouldn't need that. */
__ASM_GLOBAL_FUNC( __chkstk, "ret" );

#endif
