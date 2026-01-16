/*
 * Unit tests for MSDelta API functions
 *
 * Copyright (c) 2024 Vijay Kiran Kamuju
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
 *
 * NOTES
 *
 * Without mspatchc.dll, the inability to create test patch files under Wine
 * limits testing to the supplied small files.
 */

#include "wine/test.h"
#include "windef.h"
#include "winerror.h"

#include "msdelta.h"

static BOOL (WINAPI *pApplyDeltaA)(DELTA_FLAG_TYPE, LPCSTR, LPCSTR, LPCSTR);

static BOOL init_function_pointers(void)
{
    HMODULE msdelta = LoadLibraryA("msdelta.dll");
    if (!msdelta)
    {
        win_skip("msdelta.dll not found\n");
        return FALSE;
    }
    pApplyDeltaA = (void *)GetProcAddress(msdelta, "ApplyDeltaA");

    return TRUE;
}

static void test_ApplyDelta(void)
{
    DWORD err;

    if (!pApplyDeltaA)
        return;

    SetLastError(0xdeadbeef);
    ok(!pApplyDeltaA(0, NULL, NULL, NULL),
       "ApplyDeltaA: expected FALSE\n");
    err = GetLastError();
    ok(err == ERROR_INVALID_DATA, "Expected ERROR_INVALID_DATA, got 0x%08lx\n", err);

    SetLastError(0xdeadbeef);
    ok(!pApplyDeltaA(0, "src.tmp", NULL, NULL),
       "ApplyDeltaA: expected FALSE\n");
    err = GetLastError();
    ok(err == ERROR_FILE_NOT_FOUND, "Expected ERROR_FILE_NOT_FOUND, got 0x%08lx\n", err);

    SetLastError(0xdeadbeef);
    ok(!pApplyDeltaA(0, "src.tmp", "delta.tmp", NULL),
       "ApplyDeltaA: expected FALSE\n");
    err = GetLastError();
    ok(err == ERROR_FILE_NOT_FOUND, "Expected ERROR_FILE_NOT_FOUND, got 0x%08lx\n", err);

    SetLastError(0xdeadbeef);
    ok(!pApplyDeltaA(0, "src.tmp", "delta.tmp", "tgt.tmp"),
       "ApplyDeltaA: expected FALSE\n");
    err = GetLastError();
    ok(err == ERROR_FILE_NOT_FOUND, "Expected ERROR_FILE_NOT_FOUND, got 0x%08lx\n", err);
}

START_TEST(apply_delta)
{
    if (!init_function_pointers())
        return;

    test_ApplyDelta();
}
