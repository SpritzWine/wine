/* Avrt dll implementation
 *
 * Copyright (C) 2009 Maarten Lankhorst
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* The MMCSS entry points (AvSetMmThreadCharacteristicsW & friends) are
 * implemented in Rust -- the no_std PE staticlib rust/src_dll/avrt_pe, linked in
 * via EXTRADLLFLAGS (see Makefile.in). What remains here is just the ANSI->wide
 * shims, which widen the task name and forward to those W functions. */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "avrt.h"

static inline WCHAR *strdupAW(const char *src)
{
    int len;
    WCHAR *dst;
    if (!src) return NULL;
    len = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);
    if ((dst = malloc(len * sizeof(*dst)))) MultiByteToWideChar(CP_ACP, 0, src, -1, dst, len);
    return dst;
}

HANDLE WINAPI AvSetMmThreadCharacteristicsA(const char *name, DWORD *index)
{
    WCHAR *nameW = NULL;
    HANDLE ret;

    if (name && !(nameW = strdupAW(name)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    ret = AvSetMmThreadCharacteristicsW(nameW, index);

    free(nameW);
    return ret;
}

HANDLE WINAPI AvSetMmMaxThreadCharacteristicsA(const char *task1, const char *task2, DWORD *index)
{
    WCHAR *task1W = NULL, *task2W = NULL;
    HANDLE ret;

    if (task1 && !(task1W = strdupAW(task1)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    if (task2 && !(task2W = strdupAW(task2)))
    {
        free(task1W);
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    ret = AvSetMmMaxThreadCharacteristicsW(task1W, task2W, index);

    free(task2W);
    free(task1W);
    return ret;
}
