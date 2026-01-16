/* Unit test suite for *Information* Registry API functions
 *
 * Copyright 2024 Grigory Vasilyev
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
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "wine/test.h"
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winuser.h"
#include "winternl.h"

static void test_get_firmware_type(void)
{
    FIRMWARE_TYPE ft;
    BOOL status;

    status = GetFirmwareType(&ft);
    if (GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
    {
        skip("GetFirmwareType not implemented.\n");
        return;
    }

    ok(status == TRUE, "Expected TRUE.\n");

    ok(ft == FirmwareTypeBios || ft == FirmwareTypeUefi,
       "Expected FirmwareTypeBios or FirmwareTypeUefi, got %08x\n", ft);

    status = GetFirmwareType(NULL);
    ok(status == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected FALSE and GetLastError() == ERROR_INVALID_PARAMETER\n");
}

static BOOL EnableTokenPrivilege(HANDLE hToken, const char *pszPrivilegesName, BOOL bEnabled)
{
    LUID luid;
    TOKEN_PRIVILEGES tp;

    if (!LookupPrivilegeValueA(NULL, pszPrivilegesName, &luid))
    {
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = bEnabled ? SE_PRIVILEGE_ENABLED : 0;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL))
    {
        return FALSE;
    }

    return TRUE;
}

static void test_get_firmware_environment(void)
{
    int status;
    BOOLEAN secureboot;
    DWORD attributes;
    HANDLE hToken;
    HANDLE hProcess;
    BOOL privileged = FALSE;

    hProcess = GetCurrentProcess();
    if(hProcess && OpenProcessToken(hProcess, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)
                && EnableTokenPrivilege(hToken, SE_SYSTEM_ENVIRONMENT_NAME, TRUE))
        privileged = TRUE;

    /*
     * GetFirmwareEnvironmentVariableA
     */
    status = GetFirmwareEnvironmentVariableA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             &secureboot, sizeof(BOOLEAN));

    if(!privileged && !status && GetLastError() == ERROR_PRIVILEGE_NOT_HELD)
    {
        skip("GetFirmwareEnvironmentVariable* demand privileges SE_SYSTEM_ENVIRONMENT_NAME.\n");
        return;
    }

    if (!status && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
    {
        skip("GetFirmwareEnvironmentVariableA call NtQuerySystemEnvironmentValueEx"
             "and NtQuerySystemEnvironmentValueEx not implemented.\n");
        goto get_firmware_environment_variable_w;
    }

    if (!status && GetLastError() == ERROR_INVALID_FUNCTION)
    {
        skip("GetFirmwareEnvironmentVariableA call NtQuerySystemEnvironmentValueEx"
             " and NtQuerySystemEnvironmentValueEx returned ERROR_INVALID_FUNCTION.\n"
             "This behavior matches the behavior of Windows for non-UEFI boots and older versions of Windows.\n");
        goto get_firmware_environment_variable_w;
    }

    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0 or GetLastError() == ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableA(NULL, NULL, &secureboot, sizeof(secureboot));
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableA(NULL, "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             &secureboot, sizeof(secureboot));
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableA("SecureBoot", NULL, &secureboot, sizeof(secureboot));
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}", NULL, 0);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             &secureboot, 0);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             NULL, sizeof(secureboot));
    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0 or GetLastError() == ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableA("DummyName", "{00000000-0000-0000-0000-000000000000}",
                                             &secureboot, sizeof(secureboot));
    ok(GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    /*
     * GetFirmwareEnvironmentVariableW
     */
    get_firmware_environment_variable_w:
    status = GetFirmwareEnvironmentVariableW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             &secureboot, sizeof(BOOLEAN));

    if (!status && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
    {
        skip("GetFirmwareEnvironmentVariableW call NtQuerySystemEnvironmentValueEx"
             "and NtQuerySystemEnvironmentValueEx not implemented.\n");
        goto get_firmware_environment_variable_ex_a;
    }

    if (!status && GetLastError() == ERROR_INVALID_FUNCTION)
    {
        skip("GetFirmwareEnvironmentVariableW call NtQuerySystemEnvironmentValueEx"
             " and NtQuerySystemEnvironmentValueEx returned ERROR_INVALID_FUNCTION.\n"
             "This behavior matches the behavior of Windows for non-UEFI boots and older versions of Windows.\n");
        goto get_firmware_environment_variable_ex_a;
    }

    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0, got error %ld\n", GetLastError());

    status = GetFirmwareEnvironmentVariableW(NULL, NULL, &secureboot, sizeof(secureboot));
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableW(NULL, L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             &secureboot, sizeof(secureboot));
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableW(L"SecureBoot", NULL, &secureboot, sizeof(secureboot));
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             NULL, 0);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             &secureboot, 0);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                             NULL, sizeof(secureboot));
    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0 or GetLastError() == ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableW(L"DummyName", L"{00000000-0000-0000-0000-000000000000}",
                                             &secureboot, sizeof(secureboot));
    ok(GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    /*
     * GetFirmwareEnvironmentVariableExA
     */
    get_firmware_environment_variable_ex_a:
    status = GetFirmwareEnvironmentVariableExA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               &secureboot, sizeof(BOOLEAN), &attributes);

    if (!status && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
    {
        skip("GetFirmwareEnvironmentVariableExA call NtQuerySystemEnvironmentValueEx"
             "and NtQuerySystemEnvironmentValueEx not implemented.\n");
        goto get_firmware_environment_variable_ex_w;
    }

    if (!status && GetLastError() == ERROR_INVALID_FUNCTION)
    {
        skip("GetFirmwareEnvironmentVariableExA call NtQuerySystemEnvironmentValueEx"
             " and NtQuerySystemEnvironmentValueEx returned ERROR_INVALID_FUNCTION.\n"
             "This behavior matches the behavior of Windows for non-UEFI boots and older versions of Windows.\n");
        goto get_firmware_environment_variable_ex_w;
    }

    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0, got error %ld\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA(NULL, NULL, &secureboot, sizeof(secureboot), &attributes);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA(NULL, "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               &secureboot, sizeof(secureboot), &attributes);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA("SecureBoot", NULL, &secureboot,
                                               sizeof(secureboot), &attributes);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               NULL, 0, NULL);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               &secureboot, 0, NULL);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               &secureboot, 0, &attributes);
    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0 or GetLastError() == ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               NULL, sizeof(secureboot), NULL);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA("SecureBoot", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               NULL, sizeof(secureboot), &attributes);
    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0 or GetLastError() == ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExA("DummyName", "{00000000-0000-0000-0000-000000000000}",
                                               &secureboot, sizeof(secureboot), &attributes);
    ok(GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    /*
     * GetFirmwareEnvironmentVariableExW
     */
    get_firmware_environment_variable_ex_w:
    status = GetFirmwareEnvironmentVariableExW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               &secureboot, sizeof(BOOLEAN), &attributes);

    if (!status && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
    {
        skip("GetFirmwareEnvironmentVariableExW call NtQuerySystemEnvironmentValueEx"
             "and NtQuerySystemEnvironmentValueEx not implemented.\n");
        return;
    }

    if (!status && GetLastError() == ERROR_INVALID_FUNCTION)
    {
        skip("GetFirmwareEnvironmentVariableExW call NtQuerySystemEnvironmentValueEx"
             " and NtQuerySystemEnvironmentValueEx returned ERROR_INVALID_FUNCTION.\n"
             "This behavior matches the behavior of Windows for non-UEFI boots and older versions of Windows.\n");
        return;
    }

    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0, got error %ld\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(NULL, NULL, &secureboot, sizeof(secureboot), &attributes);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(NULL, L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               &secureboot, sizeof(secureboot), &attributes);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(L"SecureBoot", NULL, &secureboot, sizeof(secureboot), &attributes);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               NULL, 0, NULL);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               &secureboot, 0, NULL);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               &secureboot, 0, &attributes);
    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0 or GetLastError() == ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               NULL, sizeof(secureboot), NULL);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "Expected ERROR_INVALID_PARAMETER, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(L"SecureBoot", L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}",
                                               NULL, sizeof(secureboot), &attributes);
    ok(status > 0 || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected status > 0 or GetLastError() == ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());

    status = GetFirmwareEnvironmentVariableExW(L"DummyName", L"{00000000-0000-0000-0000-000000000000}",
                                               &secureboot, sizeof(secureboot), &attributes);
    ok(GetLastError() == ERROR_ENVVAR_NOT_FOUND,
       "Expected ERROR_ENVVAR_NOT_FOUND, got error %ld.\n", GetLastError());
}

START_TEST(firmware)
{
    test_get_firmware_type();
    test_get_firmware_environment();
}
