/* WinRT Windows.UI Implementation
 *
 * Copyright (C) 2025 Ignacy Kuchciński
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

#include "private.h"
#include "initguid.h"
#include "radialcontrollerinterop.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ui);

struct radialcontroller_statics
{
    IActivationFactory IActivationFactory_iface;
    IRadialControllerInterop IRadialControllerInterop_iface;
    IRadialControllerStatics IRadialControllerStatics_iface;
    LONG ref;
};

static inline struct radialcontroller_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct radialcontroller_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct radialcontroller_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    else if (IsEqualGUID( iid, &IID_IRadialControllerInterop))
    {
        *out = &impl->IRadialControllerInterop_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    else if (IsEqualGUID( iid, &IID_IRadialControllerStatics ))
    {
        *out = &impl->IRadialControllerStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct radialcontroller_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct radialcontroller_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_name %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,

    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,

    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( radialcontroller_interop, IRadialControllerInterop, struct radialcontroller_statics, IActivationFactory_iface );

static HRESULT WINAPI radialcontroller_interop_GetForWindow( IRadialControllerInterop *iface, HWND window, REFIID riid, void **radialcontroller )
{
    struct radialcontroller_statics *impl = impl_from_IRadialControllerInterop ( iface );

    TRACE( "(window %p, riid %s, radialcontroller %p)\n", window, debugstr_guid( riid ), radialcontroller );

    factory_ActivateInstance( &impl->IActivationFactory_iface, (IInspectable **)radialcontroller );
    return S_OK;
}

static const struct IRadialControllerInteropVtbl radialcontroller_interop_vtbl =
{
    radialcontroller_interop_QueryInterface,
    radialcontroller_interop_AddRef,
    radialcontroller_interop_Release,

    /* IInspectable methods */
    radialcontroller_interop_GetIids,
    radialcontroller_interop_GetRuntimeClassName,
    radialcontroller_interop_GetTrustLevel,

    /* IRadialControllerInterop methods */
    radialcontroller_interop_GetForWindow,
};

DEFINE_IINSPECTABLE( radialcontroller_statics, IRadialControllerStatics, struct radialcontroller_statics, IActivationFactory_iface );

static const struct IRadialControllerStaticsVtbl radialcontroller_statics_vtbl =
{
    radialcontroller_statics_QueryInterface,
    radialcontroller_statics_AddRef,
    radialcontroller_statics_Release,

    /* IInspectable methods */
    radialcontroller_statics_GetIids,
    radialcontroller_statics_GetRuntimeClassName,
    radialcontroller_statics_GetTrustLevel,

    /* IRadialControllerStatics methods */
};

static struct radialcontroller_statics radialcontroller_statics =
{
    {&factory_vtbl},
    {&radialcontroller_interop_vtbl},
    {&radialcontroller_statics_vtbl},
    1,
};

IActivationFactory *radialcontroller_factory = &radialcontroller_statics.IActivationFactory_iface;
