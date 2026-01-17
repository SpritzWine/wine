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
#include "radialcontrollerconfigurationinterop.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ui);

struct radialcontrollerconfiguration_statics
{
    IActivationFactory IActivationFactory_iface;
    IRadialControllerConfigurationInterop IRadialControllerConfigurationInterop_iface;
    IRadialControllerConfigurationStatics IRadialControllerConfigurationStatics_iface;
    LONG ref;
};

static inline struct radialcontrollerconfiguration_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct radialcontrollerconfiguration_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct radialcontrollerconfiguration_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    else if (IsEqualGUID( iid, &IID_IRadialControllerConfigurationInterop ))
    {
        *out = &impl->IRadialControllerConfigurationInterop_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    else if (IsEqualGUID( iid, &IID_IRadialControllerConfigurationStatics ))
    {
        *out = &impl->IRadialControllerConfigurationStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct radialcontrollerconfiguration_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct radialcontrollerconfiguration_statics *impl = impl_from_IActivationFactory( iface );
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
    FIXME( "iface %p, instance %p.\n", iface, instance );
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

DEFINE_IINSPECTABLE( radialcontrollerconfiguration_interop, IRadialControllerConfigurationInterop, struct radialcontrollerconfiguration_statics, IActivationFactory_iface );

static HRESULT WINAPI radialcontrollerconfiguration_interop_GetForWindow( IRadialControllerConfigurationInterop *iface, HWND window, REFIID riid, void **radialcontrollerconfiguration )
{
    struct radialcontrollerconfiguration_statics *impl = impl_from_IRadialControllerConfigurationInterop ( iface );

    TRACE( "(window %p, riid %s, radialcontrollerconfiguration %p)\n", window, debugstr_guid( riid ), radialcontrollerconfiguration );

    factory_ActivateInstance( &impl->IActivationFactory_iface, (IInspectable **)radialcontrollerconfiguration );
    return S_OK;
}

static const struct IRadialControllerConfigurationInteropVtbl radialcontrollerconfiguration_interop_vtbl =
{
    radialcontrollerconfiguration_interop_QueryInterface,
    radialcontrollerconfiguration_interop_AddRef,
    radialcontrollerconfiguration_interop_Release,

    /* IInspectable methods */
    radialcontrollerconfiguration_interop_GetIids,
    radialcontrollerconfiguration_interop_GetRuntimeClassName,
    radialcontrollerconfiguration_interop_GetTrustLevel,

    /* IRadialControllerConfigurationInterop methods */
    radialcontrollerconfiguration_interop_GetForWindow,
};

DEFINE_IINSPECTABLE( radialcontrollerconfiguration_statics, IRadialControllerConfigurationStatics, struct radialcontrollerconfiguration_statics, IActivationFactory_iface );

static const struct IRadialControllerConfigurationStaticsVtbl radialcontrollerconfiguration_statics_vtbl =
{
    radialcontrollerconfiguration_statics_QueryInterface,
    radialcontrollerconfiguration_statics_AddRef,
    radialcontrollerconfiguration_statics_Release,

    /* IInspectable methods */
    radialcontrollerconfiguration_statics_GetIids,
    radialcontrollerconfiguration_statics_GetRuntimeClassName,
    radialcontrollerconfiguration_statics_GetTrustLevel,

    /* IRadialControllerConfigurationStatics methods */
};

static struct radialcontrollerconfiguration_statics radialcontrollerconfiguration_statics =
{
    {&factory_vtbl},
    {&radialcontrollerconfiguration_interop_vtbl},
    {&radialcontrollerconfiguration_statics_vtbl},
    1,
};

IActivationFactory *radialcontrollerconfiguration_factory = &radialcontrollerconfiguration_statics.IActivationFactory_iface;
