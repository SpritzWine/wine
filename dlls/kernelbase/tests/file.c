/*
 * Copyright (C) 2023 Paul Gofman for CodeWeavers
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

#include <stdarg.h>
#include <stdlib.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <ioringapi.h>

#include "wine/test.h"

static HRESULT (WINAPI *pQueryIoRingCapabilities)(IORING_CAPABILITIES *);

static void test_ioring_caps(void)
{
    IORING_CAPABILITIES caps;
    HRESULT hr;

    if (!pQueryIoRingCapabilities)
    {
        win_skip("QueryIoRingCapabilities is not available, skipping tests.\n");
        return;
    }

    memset(&caps, 0xcc, sizeof(caps));
    hr = pQueryIoRingCapabilities(&caps);
    todo_wine ok(hr == S_OK, "got %#lx.\n", hr);
}

static void test_load_library_ex_flags(void)
{
    HMODULE hmod;
    DWORD last_error;

    SetLastError(0x33);

    /* Test if LoadLibraryEx can load a dll */
    hmod = LoadLibraryExW(L"kernelbase.dll", NULL /* hFile */, LOAD_LIBRARY_SEARCH_SYSTEM32 /* dwFlags */);
    ok(hmod != NULL, "LoadLibraryEx returned %p with LastError: %#lx.\n", hmod, GetLastError());

    /* When hFile is non-NULL LoadLibraryEx should return NULL and set LastError to ERROR_INVALID_PARAMETER */
    hmod = LoadLibraryExW(L"kernelbase.dll", (HANDLE)0x000003A0 /* random hFile */, 0 /* dwFlags */);
    last_error = GetLastError();
    ok(hmod == NULL && last_error == ERROR_INVALID_PARAMETER, "LoadLibraryEx returned %p with LastError: %#lx.\n", hmod, last_error);
}

START_TEST(file)
{
    HMODULE hmod;

    hmod = LoadLibraryA("kernelbase.dll");
    pQueryIoRingCapabilities = (void *)GetProcAddress(hmod, "QueryIoRingCapabilities");

    test_ioring_caps();
    test_load_library_ex_flags();
}
