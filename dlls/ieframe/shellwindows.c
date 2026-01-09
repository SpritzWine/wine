/*
 * Copyright 2025 Helix Graziani
 *
 * IShellWindows interface implementation
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

#include "ieframe.h"

#include "exdisp.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ieframe);

struct ShellWindows {
    IShellWindows IShellWindows_iface;

    LONG ref;
};

static inline struct ShellWindows *impl_from_IShellWindows(IShellWindows *iface)
{
    return CONTAINING_RECORD(iface, ShellWindows, IShellWindows_iface);
}

static HRESULT WINAPI ShellWindows_QueryInterface(IShellWindows *iface, REFIID riid, void **ppv)
{
    ShellWindows *This = impl_from_IShellWindows(iface);

    if (ppv == NULL)
        return E_POINTER;
    *ppv = NULL;

    if (IsEqualGUID(&IID_IUnknown, riid)
        || IsEqualGUID(&IID_IDispatch, riid)
        || IsEqualGUID(&IID_IShellWindows, riid))
    {
        TRACE("(%p)->(IID_IShellWindows %p)\n", This, ppv);
        *ppv = &This->IShellWindows_iface;
    }

    if (*ppv)
    {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    FIXME("(%p)->(%s %p) interface not supported\n", This, debugstr_guid(riid), ppv);

    return E_NOINTERFACE;
}

static ULONG WINAPI ShellWindows_AddRef(IShellWindows *iface)
{
    ShellWindows *This = impl_from_IShellWindows(iface);
    LONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) ref=%ld\n", This, ref);
    return ref;
}

static ULONG WINAPI ShellWindows_Release(IShellWindows *iface)
{
    ShellWindows *This = impl_from_IShellWindows(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if (!ref)
    {
        free(This);
    }

    return ref;
}

static HRESULT WINAPI ShellWindows_GetTypeInfoCount(IShellWindows *iface, UINT *pctinfo)
{
    FIXME("(%p, %p) stub!\n", iface, pctinfo);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_GetTypeInfo(IShellWindows *iface, UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo)
{
    FIXME("(%p, %d, %#lx, %p) stub!\n", iface, iTInfo, lcid, ppTInfo);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_GetIDsOfNames(IShellWindows *iface, REFIID riid, LPOLESTR *rgszNames, UINT cNames,
        LCID lcid, DISPID *rgDispId)
{
    FIXME("(%p, %s, %p, %d, %#lx, %p) stub!\n", iface, debugstr_guid(riid), rgszNames, cNames, lcid, rgDispId);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_Invoke(IShellWindows *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
        WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    FIXME("(%p, %ld, %s, %#lx, %#x, %p, %p, %p, %p) stub!\n", iface, dispIdMember, debugstr_guid(riid), lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_get_Count(IShellWindows *iface, LONG *Count)
{
    FIXME("(%p, %p) stub!\n", iface, Count);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_Item(IShellWindows *iface, VARIANT index, IDispatch **Folder)
{
    FIXME("(%p, %s, %p) stub!\n", iface, debugstr_variant(&index), Folder);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows__NewEnum(IShellWindows *iface, IUnknown **ppunk)
{
    FIXME("(%p, %p) stub!\n", iface, ppunk);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_Register(IShellWindows *iface, IDispatch *pid, LONG hWnd, int swClass, LONG *plCookie)
{
    FIXME("(%p, %p, %ld, %d, %p) stub!\n", iface, pid, hWnd, swClass, plCookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_RegisterPending(IShellWindows *iface, LONG lThreadId, VARIANT *pvarloc,
        VARIANT *pvarlocRoot, int swClass, LONG *plCookie)
{
    FIXME("(%p, %ld, %p, %p, %d, %p) stub!\n", iface, lThreadId, pvarloc, pvarlocRoot, swClass, plCookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_Revoke(IShellWindows *iface, LONG lCookie)
{
    FIXME("(%p, %ld) stub!\n", iface, lCookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_OnNavigate(IShellWindows *iface, LONG lCookie, VARIANT *pvarLoc)
{
    FIXME("(%p, %ld, %p) stub!\n", iface, lCookie, pvarLoc);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_OnActivated(IShellWindows *iface, LONG lCookie, VARIANT_BOOL fActive)
{
    FIXME("(%p, %ld, %#x) stub!\n", iface, lCookie, fActive);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_FindWindowSW(IShellWindows *iface, VARIANT *pvarLoc, VARIANT *pvarLocRoot,
        int swClass, LONG *phwnd, int swfwOptions, IDispatch **ppdispOut)
{
    FIXME("(%p, %p, %p, %d, %p, %d, %p) stub!\n", iface, pvarLoc, pvarLocRoot, swClass, phwnd, swfwOptions, ppdispOut);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_OnCreated(IShellWindows *iface, LONG lCookie, IUnknown *punk)
{
    FIXME("(%p, %ld, %p) stub!\n", iface, lCookie, punk);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellWindows_ProcessAttachDetach(IShellWindows *iface, VARIANT_BOOL fAttach)
{
    FIXME("(%p, %#x) stub!\n", iface, fAttach);
    return E_NOTIMPL;
}

static const struct IShellWindowsVtbl ShellWindowsVtbl =
{
    /* IUnknown methods */
    ShellWindows_QueryInterface,
    ShellWindows_AddRef,
    ShellWindows_Release,
    /* IDispatch methods */
    ShellWindows_GetTypeInfoCount,
    ShellWindows_GetTypeInfo,
    ShellWindows_GetIDsOfNames,
    ShellWindows_Invoke,
    /* IShellWindows methods */
    ShellWindows_get_Count,
    ShellWindows_Item,
    ShellWindows__NewEnum,
    ShellWindows_Register,
    ShellWindows_RegisterPending,
    ShellWindows_Revoke,
    ShellWindows_OnNavigate,
    ShellWindows_OnActivated,
    ShellWindows_FindWindowSW,
    ShellWindows_OnCreated,
    ShellWindows_ProcessAttachDetach
};

static HRESULT create_shellwindows(IUnknown *outer, REFIID riid, void **ppv)
{
    ShellWindows *ret;
    HRESULT hres;

    TRACE("(%p %s %p)\n", outer, debugstr_guid(riid), ppv);

    ret = calloc(1, sizeof(ShellWindows));
    if (!ret)
        return E_OUTOFMEMORY;

    ret->IShellWindows_iface.lpVtbl = &ShellWindowsVtbl;

    ret->ref = 1;

    hres = IUnknown_QueryInterface(&ret->IShellWindows_iface, riid, ppv);

    return hres;
}

HRESULT WINAPI ShellWindows_Create(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    return create_shellwindows(outer, riid, ppv);
}

