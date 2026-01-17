/*
 * Copyright (c) 2018 Alistair Leslie-Hughes
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
#include <stdarg.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winstring.h"
#include "winternl.h"

#include "initguid.h"
#include "roapi.h"
#include "roerrorapi.h"

#include "wine/test.h"

#define EXPECT_REF(obj,ref) _expect_ref((IUnknown*)obj, ref, __LINE__)
static void _expect_ref(IUnknown* obj, ULONG ref, int line)
{
    ULONG rc;
    IUnknown_AddRef(obj);
    rc = IUnknown_Release(obj);
    ok_(__FILE__,line)(rc == ref, "expected refcount %ld, got %ld\n", ref, rc);
}

static void flush_events(void)
{
    int diff = 200;
    DWORD time;
    MSG msg;

    time = GetTickCount() + diff;
    while (diff > 0)
    {
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT) == WAIT_TIMEOUT)
            break;
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        diff = time - GetTickCount();
    }
}

static void load_resource(const WCHAR *filename)
{
    DWORD written;
    HANDLE file;
    HRSRC res;
    void *ptr;

    file = CreateFileW(filename, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, 0);
    ok(file != INVALID_HANDLE_VALUE, "failed to create %s, error %lu\n", debugstr_w(filename), GetLastError());

    res = FindResourceW(NULL, filename, L"TESTDLL");
    ok(res != 0, "couldn't find resource\n");
    ptr = LockResource(LoadResource(GetModuleHandleW(NULL), res));
    WriteFile(file, ptr, SizeofResource(GetModuleHandleW(NULL), res), &written, NULL);
    ok(written == SizeofResource(GetModuleHandleW(NULL), res), "couldn't write resource\n");
    CloseHandle(file);
}

static void test_ActivationFactories(void)
{
    HRESULT hr;
    HSTRING str, str2;
    IActivationFactory *factory = NULL;
    IInspectable *inspect = NULL;
    ULONG ref;

    hr = WindowsCreateString(L"Windows.Data.Xml.Dom.XmlDocument",
                              ARRAY_SIZE(L"Windows.Data.Xml.Dom.XmlDocument") - 1, &str);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = WindowsCreateString(L"Does.Not.Exist", ARRAY_SIZE(L"Does.Not.Exist") - 1, &str2);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = RoGetActivationFactory(str2, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "Unexpected hr %#lx.\n", hr);

    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    todo_wine ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if(factory)
        IActivationFactory_Release(factory);

    hr = RoActivateInstance(str2, &inspect);
    ok(hr == REGDB_E_CLASSNOTREG, "Unexpected hr %#lx.\n", hr);

    hr = RoActivateInstance(str, &inspect);
    todo_wine ok(hr == S_OK, "UNexpected hr %#lx.\n", hr);
    if(inspect)
        IInspectable_Release(inspect);

    WindowsDeleteString(str2);
    WindowsDeleteString(str);

    hr = WindowsCreateString(L"Wine.Test.Missing", ARRAY_SIZE(L"Wine.Test.Missing") - 1, &str);
    ok(hr == S_OK, "WindowsCreateString returned %#lx.\n", hr);
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "RoGetActivationFactory returned %#lx.\n", hr);
    ok(factory == NULL, "got factory %p.\n", factory);
    WindowsDeleteString(str);
    hr = WindowsCreateString(L"Wine.Test.Class", ARRAY_SIZE(L"Wine.Test.Class") - 1, &str);
    ok(hr == S_OK, "WindowsCreateString returned %#lx.\n", hr);
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    todo_wine
    ok(hr == E_NOTIMPL || broken(hr == REGDB_E_CLASSNOTREG) /* <= w1064v1809 */,
            "RoGetActivationFactory returned %#lx.\n", hr);
    todo_wine
    ok(factory == NULL, "got factory %p.\n", factory);
    if (factory) IActivationFactory_Release(factory);
    WindowsDeleteString(str);
    hr = WindowsCreateString(L"Wine.Test.Trusted", ARRAY_SIZE(L"Wine.Test.Trusted") - 1, &str);
    ok(hr == S_OK, "WindowsCreateString returned %#lx.\n", hr);
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == S_OK || broken(hr == REGDB_E_CLASSNOTREG) /* <= w1064v1809 */,
            "RoGetActivationFactory returned %#lx.\n", hr);
    if (hr == REGDB_E_CLASSNOTREG)
        ok(!factory, "got factory %p.\n", factory);
    else
        ok(!!factory, "got factory %p.\n", factory);
    if (!factory) ref = 0;
    else ref = IActivationFactory_Release(factory);
    ok(ref == 0, "Release returned %lu\n", ref);
    WindowsDeleteString(str);

    RoUninitialize();
}

static APTTYPE check_thread_apttype;
static APTTYPEQUALIFIER check_thread_aptqualifier;
static HRESULT check_thread_hr;

static DWORD WINAPI check_apartment_thread(void *dummy)
{
    check_thread_apttype = 0xdeadbeef;
    check_thread_aptqualifier = 0xdeadbeef;
    check_thread_hr = CoGetApartmentType(&check_thread_apttype, &check_thread_aptqualifier);
    return 0;
}

#define check_thread_apartment(a) check_thread_apartment_(__LINE__, FALSE, a)
#define check_thread_apartment_broken(a) check_thread_apartment_(__LINE__, TRUE, a)
static void check_thread_apartment_(unsigned int line, BOOL broken_fail, HRESULT expected_hr_thread)
{
    HANDLE thread;

    check_thread_hr = 0xdeadbeef;
    thread = CreateThread(NULL, 0, check_apartment_thread, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    ok_(__FILE__, line)(check_thread_hr == expected_hr_thread
            || broken(broken_fail && expected_hr_thread == S_OK && check_thread_hr == CO_E_NOTINITIALIZED),
            "got %#lx, expected %#lx.\n", check_thread_hr, expected_hr_thread);
    if (SUCCEEDED(check_thread_hr))
    {
        ok_(__FILE__, line)(check_thread_apttype == APTTYPE_MTA, "got %d.\n", check_thread_apttype);
        ok_(__FILE__, line)(check_thread_aptqualifier == APTTYPEQUALIFIER_IMPLICIT_MTA, "got %d.\n", check_thread_aptqualifier);
    }
}

static HANDLE mta_init_thread_init_done_event, mta_init_thread_done_event;

static DWORD WINAPI mta_init_thread(void *dummy)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(hr == S_OK, "got %#lx.\n", hr);
    SetEvent(mta_init_thread_init_done_event);

    WaitForSingleObject(mta_init_thread_done_event, INFINITE);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI mta_init_implicit_thread(void *dummy)
{
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = WindowsCreateString(L"Does.Not.Exist", ARRAY_SIZE(L"Does.Not.Exist") - 1, &str);
    ok(hr == S_OK, "got %#lx.\n", hr);
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
    WindowsDeleteString(str);

    SetEvent(mta_init_thread_init_done_event);
    WaitForSingleObject(mta_init_thread_done_event, INFINITE);

    /* No CoUninitialize(), testing cleanup on thread exit. */
    return 0;
}

enum oletlsflags
{
    OLETLS_UUIDINITIALIZED = 0x2,
    OLETLS_DISABLE_OLE1DDE = 0x40,
    OLETLS_APARTMENTTHREADED = 0x80,
    OLETLS_MULTITHREADED = 0x100,
};

struct oletlsdata
{
    void *threadbase;
    void *smallocator;
    DWORD id;
    DWORD flags;
};

static DWORD get_oletlsflags(void)
{
    struct oletlsdata *data = NtCurrentTeb()->ReservedForOle;
    return data ? data->flags : 0;
}

static void test_implicit_mta(void)
{
    static const struct
    {
        BOOL ro_init;
        BOOL mta;
    }
    tests[] =
    {
        { FALSE, FALSE },
        { FALSE, TRUE },
        { TRUE, FALSE },
        { TRUE, TRUE },
    };
    APTTYPEQUALIFIER aptqualifier;
    IActivationFactory *factory;
    APTTYPE apttype;
    unsigned int i;
    HANDLE thread;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString(L"Does.Not.Exist", ARRAY_SIZE(L"Does.Not.Exist") - 1, &str);
    ok(hr == S_OK, "got %#lx.\n", hr);
    /* RoGetActivationFactory doesn't implicitly initialize COM. */
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == CO_E_NOTINITIALIZED, "got %#lx.\n", hr);

    check_thread_apartment(CO_E_NOTINITIALIZED);

    /* RoGetActivationFactory initializes implicit MTA. */
    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        winetest_push_context("test %u", i);
        if (tests[i].ro_init)
        {
            DWORD flags = tests[i].mta ? OLETLS_MULTITHREADED : OLETLS_APARTMENTTHREADED;

            hr = RoInitialize(tests[i].mta ? RO_INIT_MULTITHREADED : RO_INIT_SINGLETHREADED);

            flags |= OLETLS_DISABLE_OLE1DDE;
            ok((get_oletlsflags() & flags) == flags, "get_oletlsflags() = %lx\n", get_oletlsflags());
        }
        else
            hr = CoInitializeEx(NULL, tests[i].mta ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
        ok(hr == S_OK, "got %#lx.\n", hr);
        check_thread_apartment(tests[i].mta ? S_OK : CO_E_NOTINITIALIZED);
        hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
        ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
        check_thread_apartment_broken(S_OK); /* Broken on Win8. */
        hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
        ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
        check_thread_apartment_broken(S_OK); /* Broken on Win8. */
        if (tests[i].ro_init)
            RoUninitialize();
        else
            CoUninitialize();
        check_thread_apartment(CO_E_NOTINITIALIZED);
        winetest_pop_context();
    }

    mta_init_thread_init_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    mta_init_thread_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* RoGetActivationFactory references implicit MTA in a current thread
     * even if implicit MTA was already initialized: check with STA init
     * after RoGetActivationFactory(). */
    thread = CreateThread(NULL, 0, mta_init_thread, NULL, 0, NULL);
    ok(!!thread, "failed.\n");
    WaitForSingleObject(mta_init_thread_init_done_event, INFINITE);
    check_thread_apartment(S_OK);

    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
    check_thread_apartment(S_OK);

    hr = CoGetApartmentType(&apttype, &aptqualifier);
    ok(hr == S_OK, "got %#lx.\n", hr);
    ok(apttype == APTTYPE_MTA, "got %d.\n", apttype);
    ok(aptqualifier == APTTYPEQUALIFIER_IMPLICIT_MTA, "got %d.\n", aptqualifier);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = CoGetApartmentType(&apttype, &aptqualifier);
    ok(hr == S_OK, "got %#lx.\n", hr);
    ok(apttype == APTTYPE_MAINSTA, "got %d.\n", apttype);
    ok(aptqualifier == APTTYPEQUALIFIER_NONE, "got %d.\n", aptqualifier);

    SetEvent(mta_init_thread_done_event);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check_thread_apartment_broken(S_OK); /* Broken on Win8. */
    CoUninitialize();
    check_thread_apartment(CO_E_NOTINITIALIZED);

    /* RoGetActivationFactory references implicit MTA in a current thread
     * even if implicit MTA was already initialized: check with STA init
     * before RoGetActivationFactory(). */
    thread = CreateThread(NULL, 0, mta_init_thread, NULL, 0, NULL);
    ok(!!thread, "failed.\n");
    WaitForSingleObject(mta_init_thread_init_done_event, INFINITE);
    check_thread_apartment(S_OK);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
    check_thread_apartment(S_OK);

    SetEvent(mta_init_thread_done_event);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check_thread_apartment_broken(S_OK); /* Broken on Win8. */
    CoUninitialize();
    check_thread_apartment(CO_E_NOTINITIALIZED);

    /* Test implicit MTA apartment thread exit. */
    thread = CreateThread(NULL, 0, mta_init_implicit_thread, NULL, 0, NULL);
    ok(!!thread, "failed.\n");
    WaitForSingleObject(mta_init_thread_init_done_event, INFINITE);
    check_thread_apartment_broken(S_OK); /* Broken on Win8. */
    SetEvent(mta_init_thread_done_event);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check_thread_apartment(CO_E_NOTINITIALIZED);

    CloseHandle(mta_init_thread_init_done_event);
    CloseHandle(mta_init_thread_done_event);
    WindowsDeleteString(str);
}

struct unk_impl
{
    IUnknown IUnknown_iface;
    LONG ref;
};

static inline struct unk_impl *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct unk_impl, IUnknown_iface);
}

static HRESULT WINAPI unk_QueryInterface(IUnknown *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown))
    {
        *ppv = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static HRESULT WINAPI unk_no_marshal_QueryInterface(IUnknown *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_INoMarshal))
    {
        *ppv = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static HRESULT WINAPI unk_agile_QueryInterface(IUnknown *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IAgileObject))
    {
        *ppv = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI unk_AddRef(IUnknown *iface)
{
    struct unk_impl *impl = impl_from_IUnknown(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI unk_Release(IUnknown *iface)
{
    struct unk_impl *impl = impl_from_IUnknown(iface);
    return InterlockedDecrement(&impl->ref);
}

static const IUnknownVtbl unk_vtbl =
{
    unk_QueryInterface,
    unk_AddRef,
    unk_Release
};

static const IUnknownVtbl unk_no_marshal_vtbl =
{
    unk_no_marshal_QueryInterface,
    unk_AddRef,
    unk_Release
};

static const IUnknownVtbl unk_agile_vtbl =
{
    unk_agile_QueryInterface,
    unk_AddRef,
    unk_Release
};

struct test_RoGetAgileReference_thread_param
{
    enum AgileReferenceOptions option;
    RO_INIT_TYPE from_type;
    RO_INIT_TYPE to_type;
    IAgileReference *agile_reference;
    IUnknown *unk_obj;
    BOOLEAN obj_is_agile;
};

static DWORD CALLBACK test_RoGetAgileReference_thread_proc(void *arg)
{
    struct test_RoGetAgileReference_thread_param *param = (struct test_RoGetAgileReference_thread_param *)arg;
    IUnknown *unknown;
    HRESULT hr;

    winetest_push_context("%d %d %d", param->option, param->from_type, param->to_type);

    RoInitialize(param->to_type);

    unknown = NULL;
    hr = IAgileReference_Resolve(param->agile_reference, &IID_IUnknown, (void **)&unknown);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(!!unknown, "Expected pointer not NULL.\n");
    if (param->obj_is_agile)
    {
        ok(unknown == param->unk_obj, "Expected the same object.\n");
        EXPECT_REF(param->unk_obj, 4);
    }
    else if (param->from_type == RO_INIT_MULTITHREADED && param->to_type == RO_INIT_MULTITHREADED)
    {
        ok(unknown == param->unk_obj, "Expected the same object.\n");
        todo_wine_if(param->option == AGILEREFERENCE_DEFAULT)
        EXPECT_REF(param->unk_obj, param->option == AGILEREFERENCE_DEFAULT ? 5 : 6);
    }
    else
    {
        ok(unknown != param->unk_obj, "Expected a different object pointer.\n");
        todo_wine_if(param->option == AGILEREFERENCE_DEFAULT)
        EXPECT_REF(param->unk_obj, param->option == AGILEREFERENCE_DEFAULT ? 4 : 5);
        EXPECT_REF(unknown, 1);
    }
    IUnknown_Release(unknown);

    RoUninitialize();
    winetest_pop_context();
    return 0;
}

static void test_RoGetAgileReference(void)
{
    struct test_RoGetAgileReference_thread_param param;
    struct unk_impl unk_no_marshal_obj = {{&unk_no_marshal_vtbl}, 1};
    struct unk_impl unk_obj = {{&unk_vtbl}, 1};
    struct unk_impl unk_agile_obj = {{&unk_agile_vtbl}, 1};
    enum AgileReferenceOptions option;
    IAgileReference *agile_reference;
    RO_INIT_TYPE from_type, to_type;
    IUnknown *unknown, *unknown2;
    IAgileObject *agile_object;
    HANDLE thread;
    HRESULT hr;
    DWORD ret;

    for (option = AGILEREFERENCE_DEFAULT; option <= AGILEREFERENCE_DELAYEDMARSHAL; option++)
    {
        for (from_type = RO_INIT_SINGLETHREADED; from_type <= RO_INIT_MULTITHREADED; from_type++)
        {
            winetest_push_context("%d %d", option, from_type);

            hr = RoGetAgileReference(option, &IID_IUnknown, &unk_obj.IUnknown_iface, &agile_reference);
            ok(hr == CO_E_NOTINITIALIZED, "Got unexpected hr %#lx.\n", hr);

            RoInitialize(from_type);

            agile_reference = NULL;
            EXPECT_REF(&unk_obj, 1);

            /* Invalid option */
            hr = RoGetAgileReference(AGILEREFERENCE_DELAYEDMARSHAL + 1, &IID_IUnknown, &unk_obj.IUnknown_iface, &agile_reference);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

            /* Non-existent interface for the object */
            hr = RoGetAgileReference(option, &IID_IActivationFactory, &unk_obj.IUnknown_iface, &agile_reference);
            ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);

            /* Objects that implements INoMarshal */
            hr = RoGetAgileReference(option, &IID_IUnknown, &unk_no_marshal_obj.IUnknown_iface, &agile_reference);
            ok(hr == CO_E_NOT_SUPPORTED, "Got unexpected hr %#lx.\n", hr);

            /* Create agile reference object */
            hr = RoGetAgileReference(option, &IID_IUnknown, &unk_obj.IUnknown_iface, &agile_reference);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!agile_reference, "Got unexpected agile_reference.\n");
            todo_wine_if(option == AGILEREFERENCE_DEFAULT)
            EXPECT_REF(&unk_obj, 2);

            /* Check the created agile reference object has IAgileObject */
            hr = IAgileReference_QueryInterface(agile_reference, &IID_IAgileObject, (void **)&agile_object);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            IAgileObject_Release(agile_object);

            /* Resolve once */
            unknown = NULL;
            hr = IAgileReference_Resolve(agile_reference, &IID_IUnknown, (void **)&unknown);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!unknown, "Expected pointer not NULL.\n");
            ok(unknown == &unk_obj.IUnknown_iface, "Expected the same object.\n");
            todo_wine
            EXPECT_REF(&unk_obj, 3);

            /* Resolve twice */
            unknown2 = NULL;
            hr = IAgileReference_Resolve(agile_reference, &IID_IUnknown, (void **)&unknown2);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!unknown2, "Expected pointer not NULL.\n");
            ok(unknown2 == &unk_obj.IUnknown_iface, "Expected the same object.\n");
            todo_wine
            EXPECT_REF(&unk_obj, 4);

            /* Resolve in another apartment */
            for (to_type = RO_INIT_SINGLETHREADED; to_type <= RO_INIT_MULTITHREADED; to_type++)
            {
                param.option = option;
                param.from_type = from_type;
                param.to_type = to_type;
                param.agile_reference = agile_reference;
                param.unk_obj = &unk_obj.IUnknown_iface;
                param.obj_is_agile = FALSE;
                thread = CreateThread(NULL, 0, test_RoGetAgileReference_thread_proc, &param, 0, NULL);
                flush_events();
                ret = WaitForSingleObject(thread, 100);
                ok(!ret, "WaitForSingleObject failed, error %ld.\n", GetLastError());
            }

            IUnknown_Release(unknown2);
            IUnknown_Release(unknown);
            IAgileReference_Release(agile_reference);
            EXPECT_REF(&unk_obj, 1);

            hr = RoGetAgileReference(option, &IID_IUnknown, &unk_agile_obj.IUnknown_iface, &agile_reference);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!agile_reference, "Got unexpected agile_reference.\n");
            EXPECT_REF(&unk_agile_obj, 2);

            unknown = NULL;
            hr = IAgileReference_Resolve(agile_reference, &IID_IUnknown, (void **)&unknown);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!unknown, "Expected pointer not NULL.\n");
            ok(unknown == &unk_agile_obj.IUnknown_iface, "Expected the same object.\n");
            EXPECT_REF(&unk_agile_obj, 3);

            for (to_type = RO_INIT_SINGLETHREADED; to_type <= RO_INIT_MULTITHREADED; to_type++)
            {
                param.option = option;
                param.from_type = from_type;
                param.to_type = to_type;
                param.agile_reference = agile_reference;
                param.unk_obj = &unk_agile_obj.IUnknown_iface;
                param.obj_is_agile = TRUE;
                thread = CreateThread(NULL, 0, test_RoGetAgileReference_thread_proc, &param, 0, NULL);
                flush_events();
                ret = WaitForSingleObject(thread, 100);
                ok(!ret, "WaitForSingleObject failed, error %ld.\n", GetLastError());
            }

            IUnknown_Release(unknown);
            IAgileReference_Release(agile_reference);
            EXPECT_REF(&unk_obj, 1);

            RoUninitialize();
            winetest_pop_context();
        }
    }
}

static void test_RoGetErrorReportingFlags(void)
{
    UINT32 flags;
    HRESULT hr;

    hr = RoGetErrorReportingFlags(NULL);
    ok(hr == E_POINTER, "Got unexpected hr %#lx.\n", hr);

    hr = RoGetErrorReportingFlags(&flags);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(flags == RO_ERROR_REPORTING_USESETERRORINFO, "Got unexpected flag %#x.\n", flags);
}

static const RO_ERROR_REPORTING_FLAGS test_flags[] = {
    RO_ERROR_REPORTING_SUPPRESSEXCEPTIONS,
    RO_ERROR_REPORTING_FORCEEXCEPTIONS,
    RO_ERROR_REPORTING_FORCEEXCEPTIONS,
    RO_ERROR_REPORTING_USESETERRORINFO,
    RO_ERROR_REPORTING_SUPPRESSSETERRORINFO
};

#define set_error_reporting_flags(f) set_error_reporting_flags_(__LINE__, f)
static void set_error_reporting_flags_(int line, UINT32 flags)
{
    UINT32 new_flags = ~flags;
    HRESULT hr;

    hr = RoSetErrorReportingFlags(flags);
    ok_(__FILE__, line)(hr == S_OK, "RoSetErrorReportingFlags failed, hr %#lx.\n", hr);
    hr = RoGetErrorReportingFlags(&new_flags);
    ok_(__FILE__, line)(hr == S_OK, "RoGetErrorReportingFlags failed, hr %#lx.\n", hr);
    ok_(__FILE__, line)(new_flags == flags, "Got unexpected flags %#x != %#x.\n", new_flags, flags);
}

struct test_error_reporting_flags_params
{
    HANDLE event1;
    HANDLE event2;
};

/* Flags are not apartment/thread local. */
static DWORD CALLBACK test_thread_RoSetErrorReportingFlags(void *param)
{
    struct test_error_reporting_flags_params *data = param;
    UINT32 flags = 0xdeadbeef;
    HRESULT hr;
    DWORD ret;
    int i;

    hr = RoInitialize(RO_INIT_SINGLETHREADED);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    for (i = 0; i < ARRAY_SIZE(test_flags); i++)
    {
        winetest_push_context("flags=%#x", test_flags[i]);
        ret = WaitForSingleObject(data->event1, 500);
        ok(!ret, "WaitForSingleObject failed, error %lu.\n", ret);

        set_error_reporting_flags(test_flags[i]);
        ret = SetEvent(data->event2);
        ok(ret, "SetEvent failed, error %lu.\n", GetLastError());

        /* Wait for the parent thread to reset flags to RO_ERROR_REPORTING_NONE. */
        ret = WaitForSingleObject(data->event1, 500);
        ok(!ret, "WaitForSingleObject failed, error %lu.\n", ret);
        flags = 0xdeadbeef;
        hr = RoGetErrorReportingFlags(&flags);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        ok(flags == RO_ERROR_REPORTING_NONE, "Got unexpected flags %#.x\n", flags);
        winetest_pop_context();
    }

    RoUninitialize();
    return 0;
}

static void test_RoSetErrorReportingFlags(void)
{
    struct test_error_reporting_flags_params data;
    RO_INIT_TYPE type;
    HANDLE thread;
    UINT32 flags;
    DWORD ret, i;
    HRESULT hr;

    /* Pass non-existent flags */
    hr = RoSetErrorReportingFlags(RO_ERROR_REPORTING_USESETERRORINFO | 0x80);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    set_error_reporting_flags(RO_ERROR_REPORTING_NONE);

    data.event1 = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!data.event1, "CreateEventW failed, error %lu.\n", GetLastError());
    data.event2 = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!data.event2, "CreateEventW failed, error %lu.\n", GetLastError());

    for (type = RO_INIT_SINGLETHREADED; type < RO_INIT_MULTITHREADED; type++)
    {
        winetest_push_context("type=%d", type);

        thread = CreateThread(NULL, 0, test_thread_RoSetErrorReportingFlags, &data, 0, NULL);
        ok(!!thread, "CreateThread failed, error %lu.\n", GetLastError());

        for (i = 0; i < ARRAY_SIZE(test_flags); i++)
        {
            winetest_push_context("flags=%#x", test_flags[i]);
            hr = RoInitialize(type);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

            /* Flags don't change on apartment uninitialization.*/
            flags = 0xdeadbeef;
            hr = RoGetErrorReportingFlags(&flags);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(flags == RO_ERROR_REPORTING_NONE, "Got unexpected flags %#x.\n", flags);

            ret = SetEvent(data.event1);
            ok(ret, "SetEvent failed, error %lu.\n", GetLastError());
            /* Wait for the other thread to call RoSetErrorReportingFlags. */
            ret = WaitForSingleObject(data.event2, 500);
            ok(!ret, "WaitForSingleObject failed, error %lu.\n", ret);

            /* RoSetErrorReportingFlags on the other thread should reflect here as well. */
            flags = 0xdeadbeef;
            hr = RoGetErrorReportingFlags(&flags);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(flags == test_flags[i], "Got unexpected flags %#x\n", flags);

            /* Reset flags to RO_ERROR_REPORTING_NONE */
            set_error_reporting_flags(RO_ERROR_REPORTING_NONE);
            ret = SetEvent(data.event1);
            ok(ret, "SetEvent failed, error %lu.\n", GetLastError());

            RoUninitialize();

            /* Flags don't change on apartment uninitialization. */
            hr = RoGetErrorReportingFlags(&flags);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(flags == RO_ERROR_REPORTING_NONE, "Got unexpected flags %#x.\n", flags);
            winetest_pop_context();
        }

        ret = WaitForSingleObject(thread, 100);
        ok(!ret, "WaitForSingleObject failed, error %lu.\n", ret);
        CloseHandle(thread);
        winetest_pop_context();
    }

    CloseHandle(data.event1);
    CloseHandle(data.event2);

    /* Restore the default error reporting flags. */
    set_error_reporting_flags(RO_ERROR_REPORTING_USESETERRORINFO);
}

static void test_GetRestrictedErrorInfo(void)
{
    IRestrictedErrorInfo *r_info, *r_info2;
    ICreateErrorInfo *create_info;
    IErrorInfo *info;
    ULONG count;
    HRESULT hr;
    BOOL ret;

    /* Clear the current error object, if any. */
    hr = SetErrorInfo(0, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = GetRestrictedErrorInfo(&r_info);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);

    /* The ICreateErrorInfo object returned by CreateErrorInfo does not supoprt IRestrictedErrorInfo. */
    hr = CreateErrorInfo(&create_info);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = ICreateErrorInfo_QueryInterface(create_info, &IID_IErrorInfo, (void **)&info);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ICreateErrorInfo_Release(create_info);
    hr = SetErrorInfo(0, info);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    /* GetRestrictedErrorInfo should return S_FALSE if the error object does not support IRestrictedErrorInfo. */
    hr = GetRestrictedErrorInfo(&r_info);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);
    /* Nonetheless, GetRestrictedErrorInfo will still clear the current error. */
    count = IErrorInfo_Release(info);
    ok(count == 0, "Got unexpected count %lu.\n", count);
    hr = GetErrorInfo(0, &info);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);

    /* IRestrictedErrorInfo objects can only be created by the Ro* error reporting methods. */
    ret = RoOriginateError(E_INVALIDARG, NULL);
    ok(ret, "RoOriginateError failed.\n");
    hr = GetRestrictedErrorInfo(&r_info);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IRestrictedErrorInfo_QueryInterface(r_info, &IID_IErrorInfo, (void **)&info);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IRestrictedErrorInfo_Release(r_info);
    hr = SetErrorInfo(0, info);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IErrorInfo_Release(info);
    hr = GetRestrictedErrorInfo(&r_info2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(r_info2 == r_info, "Got unexpected r_info2 %p != %p.\n", r_info2, r_info);
    count = IRestrictedErrorInfo_Release(r_info2);
    ok(count == 0, "Got unexpected count %lu.\n", count);
}

/* Return the system-supplied description of an HRESULT code. If there isn't one, we only check whether error
 * descriptions are not empty strings. */
static BOOL get_hresult_message(HRESULT code, WCHAR *buf, ULONG len)
{
    return !!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, code, 0, buf, len, NULL);
}

#define test_IRestrictedErrorInfo(info, code, rest_desc, ref) \
    test_IRestrictedErrorInfo_(__LINE__, info, code, rest_desc, ref)
static void test_IRestrictedErrorInfo_(int line, IRestrictedErrorInfo *r_info, HRESULT exp_code,
                                       const WCHAR *exp_rest_desc, const WCHAR *exp_ref)
{
    BSTR desc = NULL, rest_desc = NULL, sid = NULL, ref = NULL, str;
    GUID guid = IID_IUnknown;
    HRESULT hr, code = S_OK;
    WCHAR default_msg[513];
    BOOL have_default_msg;
    const WCHAR *str2;
    IErrorInfo *info;
    ULONG count, ctx;

    have_default_msg = get_hresult_message(exp_code, default_msg, ARRAY_SIZE(default_msg));

    hr = IRestrictedErrorInfo_GetErrorDetails(r_info, &desc, &code, &rest_desc, &sid);
    ok_(__FILE__, line)(hr == S_OK, "GetErrorDetails failed, hr %#lx.\n", hr);
    if (have_default_msg)
        ok_(__FILE__, line)(desc && !wcscmp(desc, default_msg), "Got desc %s != %s.\n", debugstr_w(desc),
                            debugstr_w(default_msg));
    else
        ok_(__FILE__, line)(desc && wcslen(desc), "Got desc %s.\n", debugstr_w(desc));
    ok_(__FILE__, line)(code == exp_code, "Got unexpected code %#lx.\n", code);
    if (exp_rest_desc || have_default_msg)
    {
        str2 = exp_rest_desc ? exp_rest_desc : default_msg;
        ok_(__FILE__, line)(rest_desc && !wcscmp(rest_desc, str2), "Got rest_desc %s != %s.\n", debugstr_w(rest_desc),
                            debugstr_w(str2));
    }
    else
        ok_(__FILE__, line)(rest_desc && wcslen(rest_desc), "Got rest_desc %s.\n", debugstr_w(rest_desc));
    SysFreeString(desc);
    SysFreeString(rest_desc);

    hr = IRestrictedErrorInfo_GetReference(r_info, &ref);
    ok_(__FILE__, line)(hr == S_OK, "GetReference failed, hr %#lx.\n", hr);
    ok_(__FILE__, line)((!ref && !exp_ref) || (ref && !wcscmp(ref, exp_ref)), "Got ref %s != %s.\n", debugstr_w(ref),
                        debugstr_w(exp_ref));
    SysFreeString(ref);

    /* IRestrictedErrorInfo objects also implement IErrorInfo. */
    hr = IRestrictedErrorInfo_QueryInterface(r_info, &IID_IErrorInfo, (void **)&info);
    ok_(__FILE__, line)(hr == S_OK, "QueryInterface failed, hr %#lx.\n", hr);

    hr = IErrorInfo_GetGUID(info, &guid);
    ok_(__FILE__, line)(hr == S_OK, "GetGUID failed, hr %#lx.\n", hr);
    ok_(__FILE__, line)(IsEqualGUID(&guid, &GUID_NULL), "Got unexpected guid %s.\n", debugstr_guid(&guid));

    desc = NULL;
    hr = IErrorInfo_GetDescription(info, &desc);
    ok_(__FILE__, line)(hr == S_OK, "GetDescription failed, hr %#lx.\n", hr);
    if (have_default_msg)
        ok_(__FILE__, line)(desc && !wcscmp(desc, default_msg), "GetDescription returned desc %s != %s\n",
                            debugstr_w(desc), debugstr_w(default_msg));
    else
        ok_(__FILE__, line)(desc && wcslen(desc), "GetDescription returned desc %s.\n", debugstr_w(desc));
    SysFreeString(desc);

    str = (BSTR)0xdeadbeef;
    hr = IErrorInfo_GetSource(info, &str);
    ok_(__FILE__, line)(hr == S_OK, "GetSource failed, hr %#lx.\n", hr);
    ok_(__FILE__, line)(!str, "GetSource returned str %p.\n", debugstr_w(str));

    str = (BSTR)0xdeadbeef;
    hr = IErrorInfo_GetHelpFile(info, &str);
    ok_(__FILE__, line)(hr == S_OK, "GetHelpFile failed, hr %#lx.\n", hr);
    ok_(__FILE__, line)(!str, "GetHelpFile returned str %p.\n", debugstr_w(str));

    ctx = 0xdeadbeef;
    hr = IErrorInfo_GetHelpContext(info, &ctx);
    ok_(__FILE__, line)(hr == S_OK, "GetHelpContext failed, hr %#lx.\n", hr);
    ok_(__FILE__, line)(!ctx, "GetHelpContext returned ctx %#lx.\n", ctx);

    IErrorInfo_Release(info);
    /* GetRestrictedErrorInfo transfers ownership of the object to the caller. */
    count = IRestrictedErrorInfo_Release(r_info);
    ok_(__FILE__, line)(count == 0, "Got unexpected count %lu.\n", count);
}

static BOOL exception_caught;
static HRESULT exp_hresult;
static ULONG exp_len;
static const WCHAR *exp_msg;
/* If FALSE, don't compare the message length and string for equality. */
static BOOL have_default_msg;

static LONG WINAPI rooriginate_handler(EXCEPTION_POINTERS *ptr)
{
    const EXCEPTION_RECORD *rec = ptr->ExceptionRecord;
    const HRESULT hr = rec->ExceptionInformation[0];
    const ULONG_PTR len = rec->ExceptionInformation[1];
    const WCHAR *msg = (WCHAR *)rec->ExceptionInformation[2];

    exception_caught = TRUE;
    ok(rec->NumberParameters == 3, "Got unexpected NumberParameters %lu.\n", rec->NumberParameters);
    ok(rec->ExceptionCode == EXCEPTION_RO_ORIGINATEERROR, "Got unexpected ExceptionCode %#lx.\n", rec->ExceptionCode);
    ok(hr == exp_hresult, "Got unexpected ExceptionInformation[0] %#lx != %#lx.\n", hr, exp_hresult);
    ok(len, "Got unexpected ExceptionInformation[1] %Iu.\n", len);
    ok(msg && len == wcslen(msg), "Got unexpected ExceptionInformation[2] %s.\n", debugstr_w(msg));
    if (have_default_msg)
    {
        ok(len == exp_len, "Got unexpected ExceptionInformation[1] %Iu != %lu.\n", len, exp_len);
        ok(msg && !wcscmp(msg, exp_msg), "Got unexpected ExceptionInformation[2] %s != %s.\n", debugstr_w(msg),
           debugstr_w(exp_msg));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void test_error_reporting(void)
{
    static const HRESULT test_codes[] = {E_INVALIDARG, E_FAIL, E_BOUNDS, E_NOINTERFACE, E_ABORT, E_CHANGED_STATE, RO_E_CLOSED, 0xdeffbeef};
    static const WCHAR message_nul[] = {'W', 'i', 'n', 'e', '\0', 'H', 'Q', '\0'};
    static const WCHAR *message = L"Wine is not an emulator.";

    WCHAR message_large[600], message_trunc[513], default_msg[513];
    const BOOL debugger = IsDebuggerPresent();
    IRestrictedErrorInfo *r_info;
    void *handler;
    HRESULT hr;
    BOOL ret;
    int i;

    /* RoOriginateError will only raise a structured exception if:
     *   A debugger is attached to the process, or
     *   RO_ERROR_REPORTING_FORCEEXCEPTIONS is set.
     * In either case, setting RO_ERROR_REPORTING_SUPPRESSEXCEPTIONS will not cause an exception to be raised. */
    handler = RtlAddVectoredExceptionHandler(1, rooriginate_handler);
    ok(!!handler, "RtlAddVectoredExceptionHandler returned NULL.\n");

    /* Using non-failure HRESULT values returns FALSE. No error information is set, and no exception is thrown. */
    exception_caught = FALSE;
    ret = RoOriginateError(S_OK, NULL);
    ok(!ret, "RoOriginateError returned %d.\n", ret);
    ok(!exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
    /* RoOriginateLanguageException has the same effect as RoOriginateError. */
    exception_caught = FALSE;
    ret = RoOriginateLanguageException(S_OK, NULL, NULL);
    ok(!ret, "RoOriginateLanguageException returned %d.\n", ret);
    ok(!exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);

    set_error_reporting_flags(RO_ERROR_REPORTING_USESETERRORINFO | RO_ERROR_REPORTING_FORCEEXCEPTIONS);
    exception_caught = FALSE;
    ret = RoOriginateError(S_FALSE, NULL);
    ok(!ret, "RoOriginateError returned %d.\n", ret);
    ok(!exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
    exception_caught = FALSE;
    ret = RoOriginateLanguageException(S_FALSE, NULL, NULL);
    ok(!ret, "RoOriginateLanguageException returned %d.\n", ret);
    ok(!exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);

    wmemset(message_large, L'a', ARRAY_SIZE(message_large));
    message_large[ARRAY_SIZE(message_large) - 1] = L'\0';
    memcpy(message_trunc, message_large, sizeof(WCHAR) * 512);
    message_trunc[512] = L'\0';

   for (i = 0; i < ARRAY_SIZE(test_codes); i++)
   {
       HSTRING_BUFFER hstr_buf;
       HSTRING_HEADER hstr_hdr;
       HSTRING msg = NULL;
       IErrorInfo *info;
       WCHAR *buf;

       winetest_push_context("test_codes[%d]", i);

       default_msg[0] = L'\0';
       have_default_msg = get_hresult_message(test_codes[i], default_msg, ARRAY_SIZE(default_msg));

       set_error_reporting_flags(RO_ERROR_REPORTING_USESETERRORINFO);
       exception_caught = FALSE;
       exp_hresult = test_codes[i];
       exp_len = wcslen(default_msg);
       exp_msg = default_msg[0] ? default_msg : NULL;
       ret = RoOriginateError(test_codes[i], NULL);
       ok(ret, "RoOriginateError returned %d.\n", ret);
       ok(exception_caught == debugger, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], NULL, NULL);
       exception_caught = FALSE;
       ret = RoOriginateLanguageException(test_codes[i], NULL, NULL);
       ok(ret, "RoOriginateLanguageException returned %d.\n", ret);
       todo_wine_if(debugger)
       ok(exception_caught == debugger, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       if (hr == S_OK) test_IRestrictedErrorInfo(r_info, test_codes[i], NULL, NULL);

       /* A NULL string with a non-zero length is accepted. */
       exception_caught = FALSE;
       ret = RoOriginateError(test_codes[i], NULL);
       ok(ret, "RoOriginateError returned %d.\n", ret);
       ok(exception_caught == debugger, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], NULL, NULL);

       /* RO_ERROR_REPORTING_FORCEEXCEPTIONS overrides RO_ERROR_REPORTING_SUPPRESSEXCEPTIONS */
       set_error_reporting_flags(RO_ERROR_REPORTING_USESETERRORINFO | RO_ERROR_REPORTING_SUPPRESSEXCEPTIONS |
                                 RO_ERROR_REPORTING_FORCEEXCEPTIONS);
       exception_caught = FALSE;
       ret = RoOriginateErrorW(test_codes[i], 0, NULL);
       ok(ret, "RoOriginateErrorW returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], NULL, NULL);

       set_error_reporting_flags(RO_ERROR_REPORTING_USESETERRORINFO);
       exception_caught = FALSE;
       exp_len = wcslen(message);
       exp_msg = message;
       /* RoOriginateError with a custom error message. */
       WindowsCreateStringReference(message, wcslen(message), &hstr_hdr, &msg);
       ret = RoOriginateError(test_codes[i], msg);
       ok(ret, "RoOriginateError returned %d.\n", ret);
       ok(exception_caught == debugger, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], message, NULL);
       exception_caught = FALSE;
       ret = RoOriginateLanguageException(test_codes[i], msg, NULL);
       ok(ret, "RoOriginateLanguageException returned %d.\n", ret);
       todo_wine_if(debugger)
       ok(exception_caught == debugger, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       if(hr == S_OK) test_IRestrictedErrorInfo(r_info, test_codes[i], message, NULL);

       set_error_reporting_flags(RO_ERROR_REPORTING_USESETERRORINFO | RO_ERROR_REPORTING_FORCEEXCEPTIONS);
       exception_caught = FALSE;
       ret = RoOriginateErrorW(test_codes[i], wcslen(message), message);
       ok(ret, "RoOriginateErrorW returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], message, NULL);

       exception_caught = FALSE;
       ret = RoOriginateErrorW(test_codes[i], 0, message);
       ok(ret, "RoOriginateErrorW returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], message, NULL);

       /* Error messages longer than 512 characters are truncated. */
       exception_caught = FALSE;
       exp_len = wcslen(message_trunc);
       exp_msg = message_trunc;
       WindowsCreateStringReference(message_large, wcslen(message_large), &hstr_hdr, &msg);
       ret = RoOriginateError(test_codes[i], msg);
       ok(ret, "RoOriginateError returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], message_trunc, NULL);
       exception_caught = FALSE;
       ret = RoOriginateLanguageException(test_codes[i], msg, NULL);
       ok(ret, "RoOriginateLanguageException returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       if(hr == S_OK) test_IRestrictedErrorInfo(r_info, test_codes[i], message_trunc, NULL);

       exception_caught = FALSE;
       ret = RoOriginateErrorW(test_codes[i], wcslen(message_large), message_large);
       ok(ret, "RoOriginateErrorW returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], message_trunc, NULL);

       exception_caught = FALSE;
       ret = RoOriginateErrorW(test_codes[i], 0, message_large);
       ok(ret, "RoOriginateErrorW returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], message_trunc, NULL);

       /* RoOriginateError with a custom error message containing an embedded NUL. */
       exception_caught = FALSE;
       exp_len = wcslen(message_nul);
       exp_msg = message_nul;
       hr = WindowsPreallocateStringBuffer(ARRAY_SIZE(message_nul), &buf, &hstr_buf);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       memcpy(buf, message_nul, sizeof(message_nul));
       hr = WindowsPromoteStringBuffer(hstr_buf, &msg);
       ok(hr == S_OK, "Got unexpected hr %#lx\n", hr);
       ret = RoOriginateError(test_codes[i], msg);
       ok(ret, "RoOriginateError returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       /* MSDN says that RoOriginateError uses SetErrorInfo to set the error object for the current thread, so we
        * should be able to get it through GetErrorInfo as well. */
       hr = GetErrorInfo(0, &info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       if (hr == S_OK)
       {
           hr = IErrorInfo_QueryInterface(info, &IID_IRestrictedErrorInfo, (void **)&r_info);
           ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
           IErrorInfo_Release(info);
           test_IRestrictedErrorInfo(r_info, test_codes[i], message_nul, NULL);
       }
       exception_caught = FALSE;
       ret = RoOriginateLanguageException(test_codes[i], msg, NULL);
       ok(ret, "RoOriginateLanguageException returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       if(hr == S_OK) test_IRestrictedErrorInfo(r_info, test_codes[i], message_nul, NULL);
       WindowsDeleteString(msg);

       exception_caught = FALSE;
       ret = RoOriginateErrorW(test_codes[i], ARRAY_SIZE(message_nul), message_nul);
       ok(ret, "RoOriginateErrorW returned %d.\n", ret);
       ok(exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
       hr = GetRestrictedErrorInfo(&r_info);
       ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
       test_IRestrictedErrorInfo(r_info, test_codes[i], message_nul, NULL);

       winetest_pop_context();
   }

    /* Disable SetErrorInfo, and suppress exceptions. */
    exception_caught = FALSE;
    set_error_reporting_flags(RO_ERROR_REPORTING_NONE | RO_ERROR_REPORTING_SUPPRESSEXCEPTIONS);
    ret = RoOriginateError(E_FAIL, NULL);
    ok(ret, "RoOriginateError returned %d.\n", ret);
    ok(!exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
    r_info = (IRestrictedErrorInfo *)0xdeadbeef;
    hr = GetRestrictedErrorInfo(&r_info);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);
    ok(!r_info, "Got unexpected r_info %p\n", r_info);
    exception_caught = FALSE;
    ret = RoOriginateLanguageException(E_FAIL, NULL, NULL);
    ok(ret, "RoOriginateLanguageException returned %d.\n", ret);
    ok(!exception_caught, "Got unexpected exception_caught %d.\n", exception_caught);
    hr = GetRestrictedErrorInfo(&r_info);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);
    ret = RtlRemoveVectoredExceptionHandler(handler);
    ok(ret, "RtlRemoveVectoredExceptionHandler returned %d.\n", ret);

    /* RO_ERROR_REPORTING_SUPPRESSSETERRORINFO overrides RO_ERROR_REPORTING_USESETERRORINFO. */
    set_error_reporting_flags(RO_ERROR_REPORTING_USESETERRORINFO | RO_ERROR_REPORTING_SUPPRESSSETERRORINFO);
    ret = RoOriginateError(E_FAIL, NULL);
    ok(ret, "RoOriginateError returned %d.\n", ret);
    hr = GetRestrictedErrorInfo(&r_info);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);
    ret = RoOriginateLanguageException(E_FAIL, NULL, NULL);
    ok(ret, "RoOriginateLanguageException returned %d.\n", ret);
    hr = GetRestrictedErrorInfo(&r_info);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);

    /* Restore the default flags. */
    set_error_reporting_flags(RO_ERROR_REPORTING_USESETERRORINFO);
}

START_TEST(roapi)
{
    BOOL ret;

    load_resource(L"wine.combase.test.dll");

    test_implicit_mta();
    test_ActivationFactories();
    test_RoGetAgileReference();
    test_RoGetErrorReportingFlags();
    test_RoSetErrorReportingFlags();
    test_GetRestrictedErrorInfo();
    test_error_reporting();

    SetLastError(0xdeadbeef);
    ret = DeleteFileW(L"wine.combase.test.dll");
    todo_wine_if(!ret && GetLastError() == ERROR_ACCESS_DENIED)
    ok(ret, "Failed to delete file, error %lu\n", GetLastError());
}
