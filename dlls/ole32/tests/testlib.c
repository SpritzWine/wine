/*
 * Test library
 *
 * Copyright 2019 Nikolay Sivov
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

#if 0
#pragma makedep testdll
#endif

#include <stdio.h>
#define COBJMACROS
#define CONST_VTABLE
#include <windows.h>
#include <initguid.h>
#include "windef.h"
#include "winbase.h"
#include "objbase.h"

DEFINE_GUID(CLSID_WineOOPTest, 0x5201163f, 0x8164, 0x4fd0, 0xa1, 0xa2, 0x5d, 0x5a, 0x36, 0x54, 0xd3, 0xbd);
DEFINE_GUID(IID_Testiface7, 0x82222222, 0x1234, 0x1234, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0);
DEFINE_GUID(CLSID_creatable, 0xa2222222,0x9535,0x4fac,0x8b,0x53,0xa4,0x8c,0xa7,0xf4,0xd7,0x26);

static LONG cLocks = 0;
static LONG cRefs = 1; /* Statically created with refcount = 1 */

static void LockModule(void)
{
    InterlockedIncrement(&cLocks);
}

static void UnlockModule(void)
{
    InterlockedDecrement(&cLocks);
}

static HRESULT WINAPI Test_IClassFactory_QueryInterface(
    LPCLASSFACTORY iface,
    REFIID riid,
    LPVOID* ppvObj)
{
    if (ppvObj == NULL) return E_POINTER;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IClassFactory))
    {
        *ppvObj = iface;
        IClassFactory_AddRef(iface);
        return S_OK;
    }

    *ppvObj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Test_IClassFactory_AddRef(LPCLASSFACTORY iface)
{
    ULONG res = (ULONG)InterlockedIncrement(&cRefs);
    if (res == 2)
        LockModule();
    return res;
}

static ULONG WINAPI Test_IClassFactory_Release(LPCLASSFACTORY iface)
{
    ULONG res = (ULONG)InterlockedDecrement(&cRefs);
    if (res == 1)
        UnlockModule();
    return res;
}

static HRESULT WINAPI Test_IClassFactory_CreateInstance(
    LPCLASSFACTORY iface,
    IUnknown* pUnkOuter,
    REFIID riid,
    LPVOID* ppvObj)
{
    *ppvObj = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI Test_IClassFactory_LockServer(
    LPCLASSFACTORY iface,
    BOOL fLock)
{
    return S_OK;
}

static const IClassFactoryVtbl TestClassFactory_Vtbl =
{
    Test_IClassFactory_QueryInterface,
    Test_IClassFactory_AddRef,
    Test_IClassFactory_Release,
    Test_IClassFactory_CreateInstance,
    Test_IClassFactory_LockServer
};

static IClassFactory Test_ClassFactory = { &TestClassFactory_Vtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **obj)
{
    if (IsEqualGUID(clsid, &CLSID_WineOOPTest))
        return 0x80001234;

    if (IsEqualGUID(clsid, &IID_Testiface7))
        return 0x80001235;

    if (IsEqualGUID(clsid, &CLSID_creatable))
        return IClassFactory_QueryInterface(&Test_ClassFactory, riid, obj);

    return E_NOTIMPL;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return (cLocks == 0) ? S_OK : S_FALSE;
}
