/*
 * Copyright 2014 Martin Storsjo
 * Copyright 2016 Michael Müller
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
#include "objbase.h"
#include "initguid.h"
#include "roapi.h"
#include "roparameterizediid.h"
#include "roerrorapi.h"
#include "winstring.h"
#include "errhandlingapi.h"

#include "combase_private.h"

#include "wine/exception.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

struct activatable_class_data
{
    ULONG size;
    DWORD unk;
    DWORD module_len;
    DWORD module_offset;
    DWORD threading_model;
};

static HRESULT get_library_for_classid(const WCHAR *classid, WCHAR **out)
{
    ACTCTX_SECTION_KEYED_DATA data;
    HKEY hkey_root, hkey_class;
    DWORD type, size;
    HRESULT hr;
    WCHAR *buf = NULL;

    *out = NULL;

    /* search activation context first */
    data.cbSize = sizeof(data);
    if (FindActCtxSectionStringW(FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX, NULL,
            ACTIVATION_CONTEXT_SECTION_WINRT_ACTIVATABLE_CLASSES, classid, &data))
    {
        struct activatable_class_data *activatable_class = (struct activatable_class_data *)data.lpData;
        void *ptr = (BYTE *)data.lpSectionBase + activatable_class->module_offset;
        *out = wcsdup(ptr);
        return S_OK;
    }

    /* load class registry key */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\WindowsRuntime\\ActivatableClassId",
                      0, KEY_READ, &hkey_root))
        return REGDB_E_READREGDB;
    if (RegOpenKeyExW(hkey_root, classid, 0, KEY_READ, &hkey_class))
    {
        WARN("Class %s not found in registry\n", debugstr_w(classid));
        RegCloseKey(hkey_root);
        return REGDB_E_CLASSNOTREG;
    }
    RegCloseKey(hkey_root);

    /* load (and expand) DllPath registry value */
    if (RegQueryValueExW(hkey_class, L"DllPath", NULL, &type, NULL, &size))
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ)
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (!(buf = malloc(size)))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (RegQueryValueExW(hkey_class, L"DllPath", NULL, NULL, (BYTE *)buf, &size))
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (type == REG_EXPAND_SZ)
    {
        WCHAR *expanded;
        DWORD len = ExpandEnvironmentStringsW(buf, NULL, 0);
        if (!(expanded = malloc(len * sizeof(WCHAR))))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        ExpandEnvironmentStringsW(buf, expanded, len);
        free(buf);
        buf = expanded;
    }

    *out = buf;
    return S_OK;

done:
    free(buf);
    RegCloseKey(hkey_class);
    return hr;
}


/***********************************************************************
 *      RoInitialize (combase.@)
 */
HRESULT WINAPI RoInitialize(RO_INIT_TYPE type)
{
    switch (type) {
    case RO_INIT_SINGLETHREADED:
        return CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    default:
        FIXME("type %d\n", type);
    case RO_INIT_MULTITHREADED:
        return CoInitializeEx(NULL, COINIT_MULTITHREADED);
    }
}

/***********************************************************************
 *      RoUninitialize (combase.@)
 */
void WINAPI RoUninitialize(void)
{
    CoUninitialize();
}

/***********************************************************************
 *      RoGetActivationFactory (combase.@)
 */
HRESULT WINAPI DECLSPEC_HOTPATCH RoGetActivationFactory(HSTRING classid, REFIID iid, void **class_factory)
{
    PFNGETACTIVATIONFACTORY pDllGetActivationFactory;
    IActivationFactory *factory;
    WCHAR *library;
    HMODULE module;
    HRESULT hr;

    FIXME("(%s, %s, %p): semi-stub\n", debugstr_hstring(classid), debugstr_guid(iid), class_factory);

    if (!iid || !class_factory)
        return E_INVALIDARG;

    if (FAILED(hr = ensure_mta()))
        return hr;

    hr = get_library_for_classid(WindowsGetStringRawBuffer(classid, NULL), &library);
    if (FAILED(hr))
    {
        ERR("Failed to find library for %s\n", debugstr_hstring(classid));
        return hr;
    }

    if (!(module = LoadLibraryW(library)))
    {
        ERR("Failed to load module %s\n", debugstr_w(library));
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }

    if (!(pDllGetActivationFactory = (void *)GetProcAddress(module, "DllGetActivationFactory")))
    {
        ERR("Module %s does not implement DllGetActivationFactory\n", debugstr_w(library));
        hr = E_FAIL;
        goto done;
    }

    TRACE("Found library %s for class %s\n", debugstr_w(library), debugstr_hstring(classid));

    hr = pDllGetActivationFactory(classid, &factory);
    if (SUCCEEDED(hr))
    {
        hr = IActivationFactory_QueryInterface(factory, iid, class_factory);
        if (SUCCEEDED(hr))
        {
            TRACE("Created interface %p\n", *class_factory);
            module = NULL;
        }
        IActivationFactory_Release(factory);
    }
    else
    {
        ERR("Class %s not found in %s, hr %#lx.\n", wine_dbgstr_hstring(classid), debugstr_w(library), hr);
    }

done:
    free(library);
    if (module) FreeLibrary(module);
    return hr;
}

/***********************************************************************
 *      RoGetParameterizedTypeInstanceIID (combase.@)
 */
HRESULT WINAPI RoGetParameterizedTypeInstanceIID(UINT32 name_element_count, const WCHAR **name_elements,
                                                 const IRoMetaDataLocator *meta_data_locator, GUID *iid,
                                                 ROPARAMIIDHANDLE *hiid)
{
    FIXME("stub: %d %p %p %p %p\n", name_element_count, name_elements, meta_data_locator, iid, hiid);
    if (iid) *iid = GUID_NULL;
    if (hiid) *hiid = INVALID_HANDLE_VALUE;
    return E_NOTIMPL;
}

/***********************************************************************
 *      RoActivateInstance (combase.@)
 */
HRESULT WINAPI RoActivateInstance(HSTRING classid, IInspectable **instance)
{
    IActivationFactory *factory;
    HRESULT hr;

    FIXME("(%p, %p): semi-stub\n", classid, instance);

    hr = RoGetActivationFactory(classid, &IID_IActivationFactory, (void **)&factory);
    if (SUCCEEDED(hr))
    {
        hr = IActivationFactory_ActivateInstance(factory, instance);
        IActivationFactory_Release(factory);
    }

    return hr;
}

struct agile_reference
{
    IAgileReference IAgileReference_iface;
    enum AgileReferenceOptions option;
    IStream *marshal_stream;
    CRITICAL_SECTION cs;
    IUnknown *obj;
    BOOLEAN is_agile;
    LONG ref;
};

static HRESULT marshal_object_in_agile_reference(struct agile_reference *ref, REFIID riid, IUnknown *obj)
{
    HRESULT hr;

    hr = CreateStreamOnHGlobal(0, TRUE, &ref->marshal_stream);
    if (FAILED(hr))
        return hr;

    hr = CoMarshalInterface(ref->marshal_stream, riid, obj, MSHCTX_INPROC, NULL, MSHLFLAGS_TABLESTRONG);
    if (FAILED(hr))
    {
        IStream_Release(ref->marshal_stream);
        ref->marshal_stream = NULL;
    }
    return hr;
}

static inline struct agile_reference *impl_from_IAgileReference(IAgileReference *iface)
{
    return CONTAINING_RECORD(iface, struct agile_reference, IAgileReference_iface);
}

static HRESULT WINAPI agile_ref_QueryInterface(IAgileReference *iface, REFIID riid, void **obj)
{
    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), obj);

    if (!riid || !obj) return E_INVALIDARG;

    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IAgileObject)
        || IsEqualGUID(riid, &IID_IAgileReference))
    {
        IUnknown_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    *obj = NULL;
    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI agile_ref_AddRef(IAgileReference *iface)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI agile_ref_Release(IAgileReference *iface)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    LONG ref = InterlockedDecrement(&impl->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);

        if (impl->obj)
            IUnknown_Release(impl->obj);

        if (impl->marshal_stream)
        {
            LARGE_INTEGER zero = {0};

            IStream_Seek(impl->marshal_stream, zero, STREAM_SEEK_SET, NULL);
            CoReleaseMarshalData(impl->marshal_stream);
            IStream_Release(impl->marshal_stream);
        }
        DeleteCriticalSection(&impl->cs);
        free(impl);
    }

    return ref;
}

static HRESULT WINAPI agile_ref_Resolve(IAgileReference *iface, REFIID riid, void **obj)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    LARGE_INTEGER zero = {0};
    HRESULT hr;

    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), obj);

    if (impl->is_agile)
        return IUnknown_QueryInterface(impl->obj, riid, obj);

    EnterCriticalSection(&impl->cs);
    if (impl->option == AGILEREFERENCE_DELAYEDMARSHAL && impl->marshal_stream == NULL)
    {
        if (FAILED(hr = marshal_object_in_agile_reference(impl, riid, impl->obj)))
        {
            LeaveCriticalSection(&impl->cs);
            return hr;
        }

        IUnknown_Release(impl->obj);
        impl->obj = NULL;
    }

    if (SUCCEEDED(hr = IStream_Seek(impl->marshal_stream, zero, STREAM_SEEK_SET, NULL)))
        hr = CoUnmarshalInterface(impl->marshal_stream, riid, obj);

    LeaveCriticalSection(&impl->cs);
    return hr;
}

static const IAgileReferenceVtbl agile_ref_vtbl =
{
    agile_ref_QueryInterface,
    agile_ref_AddRef,
    agile_ref_Release,
    agile_ref_Resolve,
};

static BOOL object_has_interface(IUnknown *obj, REFIID iid)
{
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface(obj, iid, (void **)&unk);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
    return SUCCEEDED(hr);
}

/***********************************************************************
 *      RoGetAgileReference (combase.@)
 */
HRESULT WINAPI RoGetAgileReference(enum AgileReferenceOptions option, REFIID riid, IUnknown *obj,
                                   IAgileReference **agile_reference)
{
    struct agile_reference *impl;
    HRESULT hr;

    TRACE("(%d, %s, %p, %p).\n", option, debugstr_guid(riid), obj, agile_reference);

    if (option != AGILEREFERENCE_DEFAULT && option != AGILEREFERENCE_DELAYEDMARSHAL)
        return E_INVALIDARG;

    if (!InternalIsProcessInitialized())
    {
        ERR("Apartment not initialized\n");
        return CO_E_NOTINITIALIZED;
    }

    if (!object_has_interface(obj, riid))
        return E_NOINTERFACE;
    if (object_has_interface(obj, &IID_INoMarshal))
        return CO_E_NOT_SUPPORTED;

    impl = calloc(1, sizeof(*impl));
    if (!impl)
        return E_OUTOFMEMORY;

    impl->IAgileReference_iface.lpVtbl = &agile_ref_vtbl;
    impl->option = option;
    impl->is_agile = object_has_interface(obj, &IID_IAgileObject);
    impl->ref = 1;

    if (option == AGILEREFERENCE_DELAYEDMARSHAL || impl->is_agile)
    {
        impl->obj = obj;
        IUnknown_AddRef(impl->obj);
    }
    else if (option == AGILEREFERENCE_DEFAULT)
    {
        if (FAILED(hr = marshal_object_in_agile_reference(impl, riid, obj)))
        {
            free(impl);
            return hr;
        }
    }

    InitializeCriticalSection(&impl->cs);

    *agile_reference = &impl->IAgileReference_iface;
    return S_OK;
}

/***********************************************************************
 *      RoFailFastWithErrorContextInternal2 (combase.@)
 */
void WINAPI RoFailFastWithErrorContextInternal2(HRESULT error, ULONG exception_count, /* PSTOWED_EXCEPTION_INFORMATION_V2 */void *information)
{
    FIXME("%#lx, %lu, %p stub.\n", error, exception_count, information);
    RaiseFailFastException(NULL, NULL, 0);
}

/***********************************************************************
 *      RoGetApartmentIdentifier (combase.@)
 */
HRESULT WINAPI RoGetApartmentIdentifier(UINT64 *identifier)
{
    FIXME("(%p): stub\n", identifier);

    if (!identifier)
        return E_INVALIDARG;

    *identifier = 0xdeadbeef;
    return S_OK;
}

/***********************************************************************
 *      RoRegisterForApartmentShutdown (combase.@)
 */
HRESULT WINAPI RoRegisterForApartmentShutdown(IApartmentShutdown *callback,
        UINT64 *identifier, APARTMENT_SHUTDOWN_REGISTRATION_COOKIE *cookie)
{
    HRESULT hr;

    FIXME("(%p, %p, %p): stub\n", callback, identifier, cookie);

    hr = RoGetApartmentIdentifier(identifier);
    if (FAILED(hr))
        return hr;

    if (cookie)
        *cookie = (void *)0xcafecafe;
    return S_OK;
}

/***********************************************************************
 *      RoGetServerActivatableClasses (combase.@)
 */
HRESULT WINAPI RoGetServerActivatableClasses(HSTRING name, HSTRING **classes, DWORD *count)
{
    FIXME("(%p, %p, %p): stub\n", name, classes, count);

    if (count)
        *count = 0;
    return S_OK;
}

/***********************************************************************
 *      RoRegisterActivationFactories (combase.@)
 */
HRESULT WINAPI RoRegisterActivationFactories(HSTRING *classes, PFNGETACTIVATIONFACTORY *callbacks,
                                             UINT32 count, RO_REGISTRATION_COOKIE *cookie)
{
    FIXME("(%p, %p, %d, %p): stub\n", classes, callbacks, count, cookie);

    return S_OK;
}

struct restricted_error_info
{
    IRestrictedErrorInfo IRestrictedErrorInfo_iface;
    IErrorInfo IErrorInfo_iface;
    BSTR description;
    BSTR restricted_description;
    HRESULT code;
    LONG ref;
};

static inline struct restricted_error_info *impl_from_IRestrictedErrorInfo(IRestrictedErrorInfo *iface)
{
    return CONTAINING_RECORD(iface, struct restricted_error_info, IRestrictedErrorInfo_iface);
}

static HRESULT WINAPI restricted_error_info_QueryInterface(IRestrictedErrorInfo *iface, REFIID iid, void **out)
{
    struct restricted_error_info *impl = impl_from_IRestrictedErrorInfo(iface);

    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IRestrictedErrorInfo))
    {
        IRestrictedErrorInfo_AddRef((*out = &impl->IRestrictedErrorInfo_iface));
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_IErrorInfo))
    {
        IErrorInfo_AddRef((*out = &impl->IErrorInfo_iface));
        return S_OK;
    }

    *out = NULL;
    FIXME("%s not implemented, returning E_NOINTERFACE.", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI restricted_error_info_AddRef(IRestrictedErrorInfo *iface)
{
    struct restricted_error_info *impl = impl_from_IRestrictedErrorInfo(iface);
    TRACE("(%p)\n", iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI restricted_error_info_Release(IRestrictedErrorInfo *iface)
{
    struct restricted_error_info *impl = impl_from_IRestrictedErrorInfo(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    TRACE("(%p)\n", iface);

    if (!ref)
    {
        SysFreeString(impl->description);
        SysFreeString(impl->restricted_description);
        free(impl);
    }
    return ref;
}

static HRESULT WINAPI restricted_error_info_GetErrorDetails(IRestrictedErrorInfo *iface, BSTR *ret_desc, HRESULT *code,
                                                            BSTR *ret_restricted_desc, BSTR *sid)
{
    struct restricted_error_info *impl = impl_from_IRestrictedErrorInfo(iface);
    BSTR desc, restricted_desc;

    TRACE("(%p, %p, %p, %p, %p)\n", iface, ret_desc, code, ret_restricted_desc, sid);

    /* There are no terminating NUL characters, so we can use SysStringLen. */
    if (!(desc = SysAllocStringLen(impl->description, SysStringLen(impl->description)))) return E_OUTOFMEMORY;
    if (!(restricted_desc = SysAllocStringLen(impl->restricted_description, SysStringLen(impl->restricted_description))))
    {
        SysFreeString(desc);
        return E_OUTOFMEMORY;
    }
    *code = impl->code;
    *ret_desc = desc;
    *ret_restricted_desc = restricted_desc;

    return S_OK;
}

static HRESULT WINAPI restricted_error_info_GetReference(IRestrictedErrorInfo *iface, BSTR *reference)
{
    FIXME("(%p, %p): semi-stub!\n", iface, reference);
    *reference = NULL;
    return S_OK;
}

static IRestrictedErrorInfoVtbl restricted_error_info_vtbl =
{
    /* IUnknown */
    restricted_error_info_QueryInterface,
    restricted_error_info_AddRef,
    restricted_error_info_Release,
    /* IRestrictedErrorInfo */
    restricted_error_info_GetErrorDetails,
    restricted_error_info_GetReference,
};

static inline struct restricted_error_info *impl_from_IErrorInfo(IErrorInfo *iface)
{
    return CONTAINING_RECORD(iface, struct restricted_error_info, IErrorInfo_iface);
}

static HRESULT WINAPI error_info_QueryInterface(IErrorInfo *iface, REFIID iid, void **out)
{
    struct restricted_error_info *impl = impl_from_IErrorInfo(iface);
    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(iid), out);
    return IRestrictedErrorInfo_QueryInterface(&impl->IRestrictedErrorInfo_iface, iid, out);
}

static ULONG WINAPI error_info_AddRef(IErrorInfo *iface)
{
    struct restricted_error_info *impl = impl_from_IErrorInfo(iface);
    TRACE("(%p)\n", iface);
    return IRestrictedErrorInfo_AddRef(&impl->IRestrictedErrorInfo_iface);
}

static ULONG WINAPI error_info_Release(IErrorInfo *iface)
{
    struct restricted_error_info *impl = impl_from_IErrorInfo(iface);
    TRACE("(%p)\n", iface);
    return IRestrictedErrorInfo_Release(&impl->IRestrictedErrorInfo_iface);
}

static HRESULT WINAPI error_info_GetDescription(IErrorInfo *iface, BSTR *description)
{
    struct restricted_error_info *impl = impl_from_IErrorInfo(iface);

    TRACE("(%p, %p)\n", iface, description);

    *description = SysAllocStringLen(impl->description, SysStringLen(impl->description));
    return *description ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI error_info_GetGUID(IErrorInfo *iface, GUID *guid)
{
    TRACE("(%p, %p)\n", iface, guid);
    memset(guid, 0, sizeof(*guid));
    return S_OK;
}

static HRESULT WINAPI error_info_GetHelpContext(IErrorInfo *iface, DWORD *context)
{
    TRACE("(%p, %p)\n", iface, context);
    *context = 0;
    return S_OK;
}

static HRESULT WINAPI error_info_GetHelpFile(IErrorInfo *iface, BSTR *file)
{
    TRACE("(%p, %p)\n", iface, file);
    *file = NULL;
    return S_OK;
}

static HRESULT WINAPI error_info_GetSource(IErrorInfo *iface, BSTR *source)
{
    TRACE("(%p, %p)\n", iface, source);
    *source = NULL;
    return S_OK;
}

static const IErrorInfoVtbl error_info_vtbl =
{
    /* IUnknown */
    error_info_QueryInterface,
    error_info_AddRef,
    error_info_Release,
    /* IErrorInfo */
    error_info_GetGUID,
    error_info_GetSource,
    error_info_GetDescription,
    error_info_GetHelpFile,
    error_info_GetHelpContext
};

HRESULT restricted_error_info_create(HRESULT code, ULONG len_msg, const WCHAR *message, ULONG len_desc,
                                     const WCHAR *desc, IErrorInfo **info)
{
    struct restricted_error_info *impl;

    if (!(impl = calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;

    impl->IRestrictedErrorInfo_iface.lpVtbl = &restricted_error_info_vtbl;
    impl->IErrorInfo_iface.lpVtbl = &error_info_vtbl;
    impl->code = code;
    if (!(impl->description = SysAllocStringLen(desc, len_desc)))
    {
        free(impl);
        return E_OUTOFMEMORY;
    }
    /* If the caller did not provide a message, use the description. */
    if (!len_msg)
    {
        message = impl->description;
        len_msg = len_desc;
    }
    if (!(impl->restricted_description = SysAllocStringLen(message, len_msg)))
    {
        SysFreeString(impl->description);
        free(impl);
        return E_OUTOFMEMORY;
    }
    *info = &impl->IErrorInfo_iface;
    return S_OK;
}

/***********************************************************************
 *      GetRestrictedErrorInfo (combase.@)
 */
HRESULT WINAPI GetRestrictedErrorInfo(IRestrictedErrorInfo **info)
{
    IErrorInfo *error_info;
    HRESULT hr;

    TRACE("(%p)\n", info);

    *info = NULL;
    hr = get_error_info(&error_info);
    if (hr != S_OK) return hr;

    hr = IErrorInfo_QueryInterface(error_info, &IID_IRestrictedErrorInfo, (void **)info);
    IErrorInfo_Release(error_info);
    return FAILED(hr) ? S_FALSE : S_OK;
}

/***********************************************************************
 *      SetRestrictedErrorInfo (combase.@)
 */
HRESULT WINAPI SetRestrictedErrorInfo(IRestrictedErrorInfo *info)
{
    FIXME( "(%p)\n", info );
    return E_NOTIMPL;
}

/***********************************************************************
 *      RoOriginateLanguageException (combase.@)
 */
BOOL WINAPI RoOriginateLanguageException(HRESULT error, HSTRING message, IUnknown *language_exception)
{
    FIXME("%#lx, %s, %p: semi-stub\n", error, debugstr_hstring(message), language_exception);
    return RoOriginateError(error, message);
}

/***********************************************************************
 *      RoOriginateError (combase.@)
 */
BOOL WINAPI RoOriginateError(HRESULT error, HSTRING message)
{
    const WCHAR *buf;
    UINT32 len;

    TRACE("%#lx, %s\n", error, debugstr_hstring(message));

    buf = WindowsGetStringRawBuffer(message, &len);
    return RoOriginateErrorW(error, len, buf);
}

static LONG WINAPI rooriginate_handler(EXCEPTION_POINTERS *ptrs)
{
    EXCEPTION_RECORD *rec = ptrs->ExceptionRecord;
    return (rec->ExceptionCode == EXCEPTION_RO_ORIGINATEERROR) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

/***********************************************************************
 *      RoOriginateErrorW (combase.@)
 */
BOOL WINAPI RoOriginateErrorW(HRESULT error, UINT max_len, const WCHAR *message)
{
    BOOL set_error, raise_exception, ret = TRUE;
    UINT32 flags, len_msg = 0, len_desc;
    WCHAR desc[512];

    TRACE("%#lx, %u, %p\n", error, max_len, message);

    if (SUCCEEDED(error)) return FALSE;
    RoGetErrorReportingFlags(&flags); /* RoGetErrorReportingFlags is infalliable with a valid pointer. */
    /* We call SetErrorInfo if USESETERRORINFO is set and SUPPRESSSETERRORINFO is *not* set. */
    set_error = flags & RO_ERROR_REPORTING_USESETERRORINFO && !(flags & RO_ERROR_REPORTING_SUPPRESSSETERRORINFO);
    /* We raise a structured exception if a debugger is present and SUPPRESSEXCEPTIONS is not set.
     * However, FORCEEXCEPTIONS being set will always cause an exception to be raised. */
    raise_exception = (IsDebuggerPresent() && !(flags & RO_ERROR_REPORTING_SUPPRESSEXCEPTIONS)) ||
                      (flags & RO_ERROR_REPORTING_FORCEEXCEPTIONS);
    if (set_error || raise_exception)
    {
        /* Get the HRESULT description. */
        if (!(len_desc = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error, 0, desc, ARRAY_SIZE(desc), NULL)))
            len_desc = swprintf(desc, ARRAY_SIZE(desc), L"Error code '%#lx'.\r\n", error);
        /* If the caller provided a message, find the terminating NUL and truncate it to 512 characters. */
        if (message)
        {
            max_len = max_len ? min(max_len, 512) : 512;
            while (len_msg < max_len && message[len_msg]) len_msg++;
        }
    }
    if (set_error)
    {
        IErrorInfo *info = NULL;
        HRESULT hr;

        if (FAILED(restricted_error_info_create(error, len_msg, message, len_desc, desc, &info)))
            ret = FALSE;
        /* If restricted_error_info_create failed, this clears the current error object. */
        if (FAILED(hr = set_error_info(info)))
        {
            FIXME("Failed to set current error: %#lx\n", hr);
            if (info) IErrorInfo_Release(info);
            ret = FALSE;
        }
    }
    if (raise_exception)
    {
        const WCHAR *src = len_msg ? message : desc;
        ULONG len = len_msg ? len_msg : len_desc;
        WCHAR *str;

        if (!(str = malloc(sizeof(WCHAR) * (len + 1)))) return ret;
        memcpy(str, src, len * sizeof(WCHAR));
        str[len] = L'\0';

        __TRY
        {
            ULONG_PTR args[3];

            args[0] = error;
            args[1] = len;
            args[2] = (ULONG_PTR)str;
            RaiseException(EXCEPTION_RO_ORIGINATEERROR, 0, 3, args);
        }
        __EXCEPT(rooriginate_handler)
        {
        }
        __ENDTRY;
        free(str);
    }

    return ret;
}

/***********************************************************************
 *      RoReportUnhandledError (combase.@)
 */
HRESULT WINAPI RoReportUnhandledError(IRestrictedErrorInfo *info)
{
    FIXME("(%p): stub\n", info);
    return S_OK;
}

static LONG error_reporting_flags = RO_ERROR_REPORTING_USESETERRORINFO;
/***********************************************************************
 *      RoSetErrorReportingFlags (combase.@)
 */
HRESULT WINAPI RoSetErrorReportingFlags(UINT32 flags)
{
    UINT32 valid_flags = RO_ERROR_REPORTING_SUPPRESSEXCEPTIONS | RO_ERROR_REPORTING_FORCEEXCEPTIONS |
                         RO_ERROR_REPORTING_USESETERRORINFO | RO_ERROR_REPORTING_SUPPRESSSETERRORINFO;

    TRACE("(%08x)\n", flags);

    if (flags & ~valid_flags) return E_INVALIDARG;
    WriteRelease(&error_reporting_flags, flags);
    return S_OK;
}

/***********************************************************************
 *      RoGetErrorReportingFlags (combase.@)
 */
HRESULT WINAPI RoGetErrorReportingFlags(UINT32 *flags)
{
    TRACE("(%p)\n", flags);

    if (!flags)
        return E_POINTER;

    *flags = ReadAcquire(&error_reporting_flags);
    return S_OK;
}


/***********************************************************************
 *      CleanupTlsOleState (combase.@)
 */
void WINAPI CleanupTlsOleState(void *unknown)
{
    FIXME("(%p): stub\n", unknown);
}

/***********************************************************************
 *      DllGetActivationFactory (combase.@)
 */
HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    FIXME("(%s, %p): stub\n", debugstr_hstring(classid), factory);

    return REGDB_E_CLASSNOTREG;
}
