/*
 * DllMainCRTStartup default entry point
 *
 * Copyright 2019 Jacek Caban for CodeWeavers
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

#ifdef __WINE_PE_BUILD

#include <stdarg.h>
#include <stdio.h>
#include <process.h>
#include "windef.h"
#include "winbase.h"

extern _PIFV __xi_a[];
extern _PIFV __xi_z[];
extern _PVFV __xc_a[];
extern _PVFV __xc_z[];
extern _PVFV __xt_a[];
extern _PVFV __xt_z[];

static inline void initterm( _PVFV *start,_PVFV *end )
{
  _PVFV* current = start;

  while (current<end)
  {
    if (*current)
      (**current)();
    current++;
  }
}

static inline int initterm_e( _PIFV *table, _PIFV *end )
{
    int res = 0;

    while (!res && table < end) {
        if (*table)
            res = (**table)();
        table++;
    }
    return res;
}

static void do_global_ctors(void)
{
    if (initterm_e( __xi_a, __xi_z ) != 0) return;
    initterm( __xc_a, __xc_z );
}


static void do_global_dtors(void)
{
    initterm( __xt_a, __xt_z );
}

BOOL WINAPI DllMainCRTStartup( HINSTANCE inst, DWORD reason, void *reserved )
{
    BOOL ret;
    if (reason == DLL_PROCESS_ATTACH) do_global_ctors();
    ret = DllMain( inst, reason, reserved );
    if (reason == DLL_PROCESS_DETACH) do_global_dtors();
    return ret;
}

#endif
