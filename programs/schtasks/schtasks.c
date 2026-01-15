/*
 * Copyright 2012 Detlef Riekenberg
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

#define COBJMACROS

#include "initguid.h"
#include "taskschd.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(schtasks);

typedef struct _hash_args {
    WCHAR option[7];
    BOOL is_supported;
    BOOL is_single;
    union {
        BOOL enable;
        WCHAR *value;
    };
} hash_args;

static ITaskFolder *get_tasks_root_folder(void)
{
    ITaskService *service;
    ITaskFolder *root;
    VARIANT empty;
    HRESULT hres;

    hres = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                            &IID_ITaskService, (void**)&service);
    if (FAILED(hres))
        return NULL;

    V_VT(&empty) = VT_EMPTY;
    hres = ITaskService_Connect(service, empty, empty, empty, empty);
    if (FAILED(hres)) {
        FIXME("Connect failed: %08lx\n", hres);
        return NULL;
    }

    hres = ITaskService_GetFolder(service, NULL, &root);
    ITaskService_Release(service);
    if (FAILED(hres)) {
        FIXME("GetFolder failed: %08lx\n", hres);
        return NULL;
    }

    return root;
}

static IRegisteredTask *get_registered_task(const WCHAR *name)
{
    IRegisteredTask *registered_task;
    ITaskFolder *root;
    BSTR str;
    HRESULT hres;

    root = get_tasks_root_folder();
    if (!root)
        return NULL;

    str = SysAllocString(name);
    hres = ITaskFolder_GetTask(root, str, &registered_task);
    SysFreeString(str);
    ITaskFolder_Release(root);
    if (FAILED(hres)) {
        FIXME("GetTask failed: %08lx\n", hres);
        return NULL;
    }

    return registered_task;
}

static BSTR read_file_to_bstr(const WCHAR *file_name)
{
    LARGE_INTEGER file_size;
    DWORD read_size, size;
    unsigned char *data;
    HANDLE file;
    BOOL r = FALSE;
    BSTR ret;

    file = CreateFileW(file_name, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        FIXME("Could not open file\n");
        return NULL;
    }

    if (!GetFileSizeEx(file, &file_size) || !file_size.QuadPart) {
        FIXME("Empty file\n");
        CloseHandle(file);
        return NULL;
    }

    data = HeapAlloc(GetProcessHeap(), 0, file_size.QuadPart);
    if (data)
        r = ReadFile(file, data, file_size.QuadPart, &read_size, NULL);
    CloseHandle(file);
    if (!r) {
        FIXME("Read failed\n");
        HeapFree(GetProcessHeap(), 0, data);
        return NULL;
    }

    if (read_size > 2 && data[0] == 0xff && data[1] == 0xfe) { /* UTF-16 BOM */
        ret = SysAllocStringLen((const WCHAR *)(data + 2), (read_size - 2) / sizeof(WCHAR));
    } else {
        size = MultiByteToWideChar(CP_ACP, 0, (const char *)data, read_size, NULL, 0);
        ret = SysAllocStringLen(NULL, size);
        if (ret)
            MultiByteToWideChar(CP_ACP, 0, (const char *)data, read_size, ret, size);
    }
    HeapFree(GetProcessHeap(), 0, data);

    return ret;
}

static int search_str(WCHAR *option, const WCHAR **strlist, int count)
{
    int i;

    for (i = 0; i < count; i++) {
       if (!wcsicmp(option, strlist[i]))
           return i;
    }

    return -1;
}

static int search_option(WCHAR *option, hash_args inputs[], int icount)
{
    int i, ret;
    const WCHAR **optlist;

    optlist = malloc(icount * sizeof(WCHAR *));
    for (i = 0; i < icount; i++)
       optlist[i] = wcsdup(inputs[i].option);
    ret = search_str(option, optlist, icount);
    free(optlist);

    return ret;
}

static BOOL check_args(int argc, WCHAR *argv[], hash_args inputs[], int icount)
{
    int index;

    while (argc) {
        index = search_option(argv[0], inputs, icount);
        if (index != -1) {
             if (inputs[index].is_single) {
                 inputs[index].enable = TRUE;
                 argc--;
                 argv++;
            } else {
                 if (argc < 2 || !wcsncmp(argv[1], L"/", 1)) {
                     ERR("Missing %s value\n", debugstr_w(inputs[index].option));
                     return FALSE;
                 }
                 if (inputs[index].value) {
                     ERR("Duplicated %s argument\n", debugstr_w(inputs[index].option));
                     return FALSE;
                 }
                 inputs[index].value = argv[1];
                 argc -= 2;
                 argv += 2;
             }
             if (!inputs[index].is_supported)
                 FIXME("Unsupported %s option %s\n", debugstr_w(inputs[index].option),debugstr_w(inputs[index].value));
        } else {
             FIXME("Unsupported arguments %s\n", debugstr_w(argv[0]));
             return FALSE;
        }
    }
    return TRUE;
}

static int change_command(int argc, WCHAR *argv[])
{
    hash_args change_args[3] = { {L"/tn",     TRUE, FALSE},
                                 {L"/tr",     TRUE, FALSE},
                                 {L"/enable", TRUE, TRUE} };
    IRegisteredTask *task;
    HRESULT hres;

    if (!check_args(argc, argv, change_args, 3 ))
        return 1;

    if (!change_args[0].value) {
        ERR("Missing /tn option\n");
        return 1;
    }

    if (!change_args[2].enable && !change_args[1].value) {
        ERR("Missing change options\n");
        return 1;
    }

    task = get_registered_task(change_args[0].value);
    if (!task)
        return 1;

    if (change_args[2].enable) {
        hres = IRegisteredTask_put_Enabled(task, VARIANT_TRUE);
        if (FAILED(hres)) {
            IRegisteredTask_Release(task);
            FIXME("put_Enabled failed: %08lx\n", hres);
            return 1;
        }
    }

    IRegisteredTask_Release(task);
    return 0;
}

static int create_command(int argc, WCHAR *argv[])
{
    static const WCHAR *sc_values[] = { L"minute",  L"hourly",  L"daily",
                                        L"weekly",  L"monthly", L"once",
                                        L"onstart", L"onlogon", L"onidle",
                                        L"onevent" };
    hash_args create_args[7] = { {L"/tn",  TRUE,  FALSE},
                                 {L"/xml", TRUE,  FALSE},
                                 {L"/f",   TRUE,  TRUE},
                                 {L"/tr",  FALSE, FALSE},
                                 {L"/sc",  FALSE, FALSE},
                                 {L"/rl",  FALSE, FALSE},
                                 {L"/ru",  FALSE, FALSE} };
    ITaskFolder *root = NULL;
    LONG flags = TASK_CREATE;
    IRegisteredTask *task;
    VARIANT empty;
    BSTR str, xml;
    HRESULT hres;

    if (!check_args(argc, argv, create_args, 7 ))
        return E_FAIL;

    if (!create_args[0].value) {
        ERR("Missing /tn argument\n");
        return E_FAIL;
    }

    if (create_args[2].enable) flags = TASK_CREATE_OR_UPDATE;

    if (!create_args[3].value && !create_args[4].value) {
        if (!create_args[1].value) {
            ERR("Missing /xml argument\n");
            return E_FAIL;
        } else {
            xml = read_file_to_bstr(create_args[1].value);
            if (!xml)
                return 1;

            root = get_tasks_root_folder();
            if (!root) {
                SysFreeString(xml);
                return 1;
            }

            V_VT(&empty) = VT_EMPTY;
            str = SysAllocString(create_args[0].value);
            hres = ITaskFolder_RegisterTask(root, str, xml, flags, empty, empty,
                                            TASK_LOGON_NONE, empty, &task);

            SysFreeString(str);
            SysFreeString(xml);
            ITaskFolder_Release(root);
            if (FAILED(hres))
                return 1;

            IRegisteredTask_Release(task);
            return 0;
        }
    } else {
        if (create_args[1].value) {
            ERR("/xml option can only be used with /ru /f /tn\n");
            return E_FAIL;
        }

        if (!create_args[3].value) {
            ERR("Missing /tr argument\n");
            return E_FAIL;
        } else {
            if (!create_args[4].value) {
                ERR("Missing /sc argument\n");
                return E_FAIL;
            }

            if (search_str(create_args[4].value, sc_values, 10) == -1) {
                ERR("Invalid schedule type %s\n", debugstr_w(create_args[4].value));
                return E_FAIL;
            }
        }
        return 0;
    }

    return E_FAIL;
}

static int delete_command(int argc, WCHAR *argv[])
{
    hash_args delete_args[2] = { {L"/tn",  TRUE, FALSE},
                                 {L"/f",   TRUE, TRUE} };
    ITaskFolder *root = NULL;
    BSTR str;
    HRESULT hres;

    if (!check_args(argc, argv, delete_args, 2 ))
        return 1;

    if (!delete_args[0].value) {
        ERR("Missing /tn argument\n");
        return 1;
    }

    root = get_tasks_root_folder();
    if (!root)
        return 1;

    str = SysAllocString(delete_args[0].value);
    hres = ITaskFolder_DeleteTask(root, str, 0);
    SysFreeString(str);
    ITaskFolder_Release(root);
    if (FAILED(hres))
        return 1;

    return 0;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    int i, ret = 0;

    for (i = 0; i < argc; i++)
        TRACE(" %s", wine_dbgstr_w(argv[i]));
    TRACE("\n");

    CoInitialize(NULL);

    if (argc < 2)
        FIXME("Print current tasks state\n");
    else if (!wcsicmp(argv[1], L"/change"))
        ret = change_command(argc - 2, argv + 2);
    else if (!wcsicmp(argv[1], L"/create"))
        ret = create_command(argc - 2, argv + 2);
    else if (!wcsicmp(argv[1], L"/delete"))
        ret = delete_command(argc - 2, argv + 2);
    else
    {
        FIXME("Unsupported command %s\n", debugstr_w(argv[1]));
        ret = 1;
    }

    CoUninitialize();
    return ret;
}
