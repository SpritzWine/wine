/*
 * Modules
 *
 * Copyright 1995 Alexandre Julliard
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


#ifdef __arm64ec__

/* defined in "module.c" */

#elif defined(__x86_64__)

/*
 * Work around a Delphi bug on x86_64.  When delay loading a symbol,
 * Delphi saves rcx, rdx, r8 and r9 to the stack.  It then calls
 * GetProcAddress(), pops the saved registers and calls the function.
 * This works fine if all of the parameters are ints.  However, since
 * it does not save xmm0 - 3, it relies on GetProcAddress() preserving
 * these registers if the function takes floating point parameters.
 * This wrapper saves xmm0 - 3 to the stack.
 */
__ASM_GLOBAL_FUNC( GetProcAddress,
                   ".byte 0x48\n\t"  /* hotpatch prolog */
                   "pushq %rbp\n\t"
                   __ASM_SEH(".seh_pushreg %rbp\n\t")
                   __ASM_CFI(".cfi_adjust_cfa_offset 8\n\t")
                   __ASM_CFI(".cfi_rel_offset %rbp,0\n\t")
                   "movq %rsp,%rbp\n\t"
                   __ASM_SEH(".seh_setframe %rbp,0\n\t")
                   __ASM_CFI(".cfi_def_cfa_register %rbp\n\t")
                   __ASM_SEH(".seh_endprologue\n\t")
                   "subq $0x60,%rsp\n\t"
                   "andq $~15,%rsp\n\t"
                   "movaps %xmm0,0x20(%rsp)\n\t"
                   "movaps %xmm1,0x30(%rsp)\n\t"
                   "movaps %xmm2,0x40(%rsp)\n\t"
                   "movaps %xmm3,0x50(%rsp)\n\t"
                   "call " __ASM_NAME("get_proc_address") "\n\t"
                   "movaps 0x50(%rsp), %xmm3\n\t"
                   "movaps 0x40(%rsp), %xmm2\n\t"
                   "movaps 0x30(%rsp), %xmm1\n\t"
                   "movaps 0x20(%rsp), %xmm0\n\t"
                   "leaq 0(%rbp),%rsp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register %rsp\n\t")
                   "popq %rbp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset -8\n\t")
                   __ASM_CFI(".cfi_same_value %rbp\n\t")
                   "ret" )

#endif /* __x86_64__ */
