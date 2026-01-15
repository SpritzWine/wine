/*
 * Copyright 2016 Alistair Leslie-Hughes
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
#include <stdio.h>

#include "windows.h"
#include "ole2.h"
#include "oleauto.h"
#include "olectl.h"
#include "dispex.h"
#include "winsock2.h"
#include "iphlpapi.h"

#include "wine/test.h"
#include "wine/heap.h"

#include "netfw.h"
#include "natupnp.h"

static ULONG get_refcount(IUnknown *unk)
{
    IUnknown_AddRef(unk);
    return IUnknown_Release(unk);
}

typedef struct
{
    const WCHAR *application_name;
    const WCHAR *description;
    const WCHAR *grouping;
    const WCHAR *interface_types;
    const WCHAR *local_addresses;
    const WCHAR *local_ports;
    const WCHAR *name;
    const WCHAR *remote_addresses;
    const WCHAR *remote_ports;
    const WCHAR *service_name;
    const WCHAR *interface_name;
    NET_FW_ACTION action;
    LONG protocol;
    VARIANT_BOOL enabled;
    VARIANT_BOOL edge_traversal;
    LONG profiles;
    NET_FW_RULE_DIRECTION direction;
} rule_test;

#define compare_rule(rule, rule_expected) \
    compare_rule_(rule, rule_expected, __FILE__, __LINE__)

static void compare_rule_(rule_test *rule, rule_test *rule_expected, const char *file, int line)
{
    BOOL success;

    success = !lstrcmpW(rule->application_name, rule_expected->application_name);
    ok_(file, line)(success, "application_name: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->application_name), wine_dbgstr_w(rule->application_name));

    success = !lstrcmpW(rule->description, rule_expected->description);
    ok_(file, line)(success, "description: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->description), wine_dbgstr_w(rule->description));

    success = !lstrcmpW(rule->grouping, rule_expected->grouping);
    ok_(file, line)(success, "grouping: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->grouping), wine_dbgstr_w(rule->grouping));

    success = !lstrcmpW(rule->interface_types, rule_expected->interface_types);
    ok_(file, line)(success, "interface_types: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->interface_types), wine_dbgstr_w(rule->interface_types));

    success = !lstrcmpW(rule->local_addresses, rule_expected->local_addresses);
    ok_(file, line)(success, "local_addresses: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->local_addresses), wine_dbgstr_w(rule->local_addresses));

    success = !lstrcmpW(rule->local_ports, rule_expected->local_ports);
    ok_(file, line)(success, "local_ports: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->local_ports), wine_dbgstr_w(rule->local_ports));

    success = !lstrcmpW(rule->name, rule_expected->name);
    ok_(file, line)(success, "name: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->name), wine_dbgstr_w(rule->name));

    success = !lstrcmpW(rule->remote_addresses, rule_expected->remote_addresses);
    ok_(file, line)(success, "remote_addresses: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->remote_addresses), wine_dbgstr_w(rule->remote_addresses));

    success = !lstrcmpW(rule->remote_ports, rule_expected->remote_ports);
    ok_(file, line)(success, "remote_ports: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->remote_ports), wine_dbgstr_w(rule->remote_ports));

    success = !lstrcmpW(rule->service_name, rule_expected->service_name);
    ok_(file, line)(success, "service_name: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->service_name), wine_dbgstr_w(rule->service_name));

    success = !lstrcmpW(rule->interface_name, rule_expected->interface_name);
    ok_(file, line)(success, "interface_name: Expected %s, got %s\n", wine_dbgstr_w(rule_expected->interface_name), wine_dbgstr_w(rule->interface_name));


    ok_(file, line)(rule->action == rule_expected->action, "action: Expected %d, got %d\n", rule_expected->action, rule->action);
    ok_(file, line)(rule->protocol == rule_expected->protocol, "protocol: Expected %ld, got %ld\n", rule_expected->protocol, rule->protocol);
    ok_(file, line)(rule->enabled == rule_expected->enabled, "enabled: Expected %d, got %d\n", rule_expected->enabled, rule->enabled);
    ok_(file, line)(rule->edge_traversal == rule_expected->edge_traversal, "edge_traversal: Expected %d, got %d\n", rule_expected->edge_traversal, rule->edge_traversal);
    ok_(file, line)(rule->profiles == rule_expected->profiles, "profiles: Expected %ld, got %ld\n", rule_expected->profiles, rule->profiles);
    ok_(file, line)(rule->direction == rule_expected->direction, "direction: Expected %d, got %d\n", rule_expected->direction, rule->direction);
}

static void append_rule_entry(const WCHAR** entry, const WCHAR* append, WCHAR *rule_buffer, DWORD *next_buffer_pos, DWORD rule_buffer_len)
{
    if(*entry == NULL)
        *entry = append;
    else
    {
        DWORD entry_len = lstrlenW(*entry);
        DWORD len = entry_len + lstrlenW(append) + 2; /* Account for comma and null terminator */
        WCHAR buffer[1024] = {0};
        if (*next_buffer_pos + len >= rule_buffer_len || len >= ARRAY_SIZE(buffer))
        {
            ok(0, "Buffer too small!\n");
            return;
        }
        lstrcatW(buffer, *entry);
        lstrcatW(buffer, L",");
        lstrcatW(buffer, append);
        if (*entry + entry_len + 1 == rule_buffer + *next_buffer_pos) /* Already at end of buffer */
        {
            /* Just extend */
            memcpy((WCHAR*)*entry, buffer, len * sizeof(WCHAR));
            *next_buffer_pos += lstrlenW(append) + 1;
        }
        else
        {
            memcpy(rule_buffer + *next_buffer_pos, buffer, len * sizeof(WCHAR));
            *entry = rule_buffer + *next_buffer_pos;
            *next_buffer_pos += len;
        }
    }
}

static void parse_rule_entry_from_registry(rule_test *rule, const WCHAR *left, const WCHAR *right, WCHAR *rule_buffer, DWORD *next_buffer_pos, DWORD rule_buffer_len)
{
    const WCHAR* ignore[] = {
        L"ICMP4", L"ICMP6", L"Defer", L"LUAuth", L"LUOwn", L"TTK", L"TTK2_22", L"TTK2_27", L"TTK2_28", L"RA42", L"RA62",
        L"LPort2_20", L"AppPkgId", L"Platform", L"Platform2",
    };
    if (!lstrcmpW(left, L"Name"))
        rule->name = right;
    else if (!lstrcmpW(left, L"App"))
        rule->application_name = right;
    else if (!lstrcmpW(left, L"EmbedCtxt"))
        rule->grouping = right;
    else if (!lstrcmpW(left, L"Active"))
    {
        if (!lstrcmpW(right, L"TRUE"))
            rule->enabled = VARIANT_TRUE;
        else if (!lstrcmpW(right, L"FALSE"))
            rule->enabled = VARIANT_FALSE;
        else
            ok(0, "Invalid Action value: %s\n", wine_dbgstr_w(right));
    }
    else if (!lstrcmpW(left, L"LPort"))
        append_rule_entry(&rule->local_ports, right, rule_buffer, next_buffer_pos, rule_buffer_len);
    else if (!lstrcmpW(left, L"RPort"))
        append_rule_entry(&rule->remote_ports, right, rule_buffer, next_buffer_pos, rule_buffer_len);
    else if (!lstrcmpW(left, L"Svc"))
        rule->service_name = right;
    else if (!lstrcmpW(left, L"Protocol"))
        rule->protocol = wcstol(right, 0, 10);
    else if (!lstrcmpW(left, L"Edge"))
    {
        if (!lstrcmpW(right, L"TRUE"))
            rule->edge_traversal = VARIANT_TRUE;
        else if (!lstrcmpW(right, L"FALSE"))
            rule->edge_traversal = VARIANT_FALSE;
        else
            ok(0, "Invalid Edge value: %s\n", wine_dbgstr_w(right));
    }
    else if (!lstrcmpW(left, L"Action"))
    {
        if (!lstrcmpW(right, L"Allow"))
            rule->action = NET_FW_ACTION_ALLOW;
        else if (!lstrcmpW(right, L"Block"))
            rule->action = NET_FW_ACTION_BLOCK;
        else
            ok(0, "Invalid Action value: %s\n", wine_dbgstr_w(right));
    }
    else if (!lstrcmpW(left, L"Dir"))
    {
        if (!lstrcmpW(right, L"In"))
           rule->direction = NET_FW_RULE_DIR_IN;
        else if (!lstrcmpW(right, L"Out"))
            rule->direction = NET_FW_RULE_DIR_OUT;
        else
           ok(0, "Invalid Direction value: %s\n", wine_dbgstr_w(right));
    }
    else if (!lstrcmpW(left, L"Profile"))
    {
        if (!lstrcmpW(right, L"Private"))
            rule->profiles |= NET_FW_PROFILE2_PRIVATE;
        else if (!lstrcmpW(right, L"Public"))
            rule->profiles |= NET_FW_PROFILE2_PUBLIC;
        else if (!lstrcmpW(right, L"Domain"))
            rule->profiles |= NET_FW_PROFILE2_DOMAIN;
        else
            ok(0, "Invalid Profile value: %s\n", wine_dbgstr_w(right));
    }
    else if (!lstrcmpW(left, L"Desc"))
        rule->description = right;
    else if (!lstrcmpW(left, L"RA4"))
        rule->remote_addresses = right;
    else if (!lstrcmpW(left, L"RA6"))
        rule->remote_addresses = right;
    else if (!lstrcmpW(left, L"LA4"))
        rule->local_addresses = right;
    else if (!lstrcmpW(left, L"LA6"))
        rule->local_addresses = right;
    else if (!lstrcmpW(left, L"IF"))
    {
        IP_ADAPTER_ADDRESSES *adapters = NULL;
        ULONG len = 0;
        BOOL found = FALSE;
        HRESULT hr;
        char buffer[100];

        hr = GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &len);
        ok(hr == ERROR_NO_DATA || hr == ERROR_BUFFER_OVERFLOW, "GetAdaptersAddresses returned %08lx\n", hr);
        if(hr == ERROR_NO_DATA)
            return;

        adapters = heap_alloc(len);
        hr = GetAdaptersAddresses(AF_UNSPEC, 0, NULL, adapters, &len);
        ok(hr == ERROR_SUCCESS, "GetAdaptersAddresses returned %08lx\n", hr);

        WideCharToMultiByte(CP_ACP, 0, right, -1, buffer, sizeof(buffer), NULL, NULL);

        while (adapters)
        {
            if (!lstrcmpA(adapters->AdapterName, buffer))
            {
                rule->interface_name = adapters->FriendlyName;
                found = TRUE;
            }
            adapters = adapters->Next;
        }

        ok (found, "Can't find interface: %s\n", wine_dbgstr_w(right));
    }
    else if (!lstrcmpW(left, L"LPort2_10"))
        append_rule_entry(&rule->local_ports, right, rule_buffer, next_buffer_pos, rule_buffer_len);
    else if (!lstrcmpW(left, L"RPort2_10"))
        append_rule_entry(&rule->remote_ports, right, rule_buffer, next_buffer_pos, rule_buffer_len);
    else if (!lstrcmpW(left, L"IFType"))
        rule->interface_types = right;
    else
    {
        for (int i = 0; i < ARRAY_SIZE(ignore); i++)
        {
            if (!lstrcmpW(left, ignore[i]))
                return;
        }
        ok(0, "Unhandled entry %s = %s\n", wine_dbgstr_w(left), wine_dbgstr_w(right));
    }
}

static void parse_rule_from_registry(rule_test *rule, WCHAR *rule_buffer, DWORD next_buffer_pos, DWORD rule_buffer_len)
{
    /* Skip version */
    WCHAR *rule_text = wcschr(rule_buffer, '|') + 1;
    do
    {
        int len_entry = wcschr(rule_text, '|') - rule_text;

        /* Parse left/right */
        int len_left = wcschr(rule_text, '=') - rule_text;
        rule_text[len_left] = 0;
        rule_text[len_entry] = 0;
        parse_rule_entry_from_registry(rule, rule_text, rule_text + len_left + 1, rule_buffer, &next_buffer_pos, rule_buffer_len);

        rule_text += len_entry + 1;
    } while (*rule_text);
}

static BOOL read_rule_from_registry(rule_test *rule, const WCHAR *target_name, WCHAR *rule_buffer, DWORD rule_buffer_len)
{
    HKEY key;
    int i = 0;
    WCHAR name[200];
    DWORD type = REG_SZ;
    DWORD data_len = rule_buffer_len;
    DWORD name_len = sizeof(name);
    static const char *path = "SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules";
    LSTATUS status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key, NULL);
    ok(status == 0, "RegCreateKeyExA failed: %ld\n", status);

    memset(rule, 0, sizeof(rule_test));

    if (status)
        return FALSE;

    while ((status = RegEnumValueW(key, i, name, &name_len, NULL, &type, (BYTE*)rule_buffer, &data_len)) == ERROR_SUCCESS)
    {
        rule_buffer[data_len] = 0;

        memset(rule, 0, sizeof(rule_test));
        parse_rule_from_registry(rule, rule_buffer, data_len + 1, rule_buffer_len);
        if (!lstrcmpW(rule->name, target_name))
        {
            RegCloseKey(key);
            return TRUE;
        }
        name_len = sizeof(name);
        data_len = rule_buffer_len;
        i++;
    }
    ok(status == ERROR_NO_MORE_ITEMS, "Got: %ld\n", status);
    RegCloseKey(key);
    return FALSE;
}

static void fill_rule(INetFwRule *rule, rule_test *rule_info)
{
    HRESULT hr;
    BSTR application_name = SysAllocString(rule_info->application_name);
    BSTR description      = SysAllocString(rule_info->description);
    BSTR grouping         = SysAllocString(rule_info->grouping);
    BSTR interface_types  = SysAllocString(rule_info->interface_types);
    BSTR local_addresses  = SysAllocString(rule_info->local_addresses);
    BSTR local_ports      = SysAllocString(rule_info->local_ports);
    BSTR name             = SysAllocString(rule_info->name);
    BSTR remote_addresses = SysAllocString(rule_info->remote_addresses);
    BSTR remote_ports     = SysAllocString(rule_info->remote_ports);
    BSTR service_name     = SysAllocString(rule_info->service_name);

    hr = INetFwRule_put_Name(rule, name);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_Grouping(rule, grouping);
    ok(hr == S_OK, "Got %08lx\n", hr);

    hr = INetFwRule_put_Protocol(rule, rule_info->protocol);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_Action(rule, rule_info->action);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_ApplicationName(rule, application_name);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_Description(rule, description);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_Direction(rule, NET_FW_RULE_DIR_IN);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_EdgeTraversal(rule, rule_info->edge_traversal);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_Enabled(rule, rule_info->enabled);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_InterfaceTypes(rule, interface_types);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_LocalAddresses(rule, local_addresses);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_LocalPorts(rule, local_ports);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_Profiles(rule, rule_info->profiles);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_RemoteAddresses(rule, remote_addresses);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_RemotePorts(rule, remote_ports);
    ok(hr == S_OK, "Got %08lx\n", hr);
    hr = INetFwRule_put_ServiceName(rule, service_name);

    if (rule_info->interface_name)
    {
        VARIANT interfaces;
        SAFEARRAY *interface_list;
        LONG interface_index = 0;
        VARIANT variant_interface_name;
        BSTR interface_name = SysAllocString(rule_info->interface_name);

        variant_interface_name.n1.n2.vt = VT_BSTR;
        variant_interface_name.n1.n2.n3.bstrVal = interface_name;
        interface_list = SafeArrayCreateVector(VT_VARIANT, 0, 1);
        ok(interface_list != NULL, "SafeArrayCreateVector failed\n");
        hr = SafeArrayPutElement(interface_list, &interface_index, &variant_interface_name);
        ok(hr == S_OK, "Got %08lx\n", hr);

        interfaces.n1.n2.vt = VT_ARRAY | VT_VARIANT;
        interfaces.n1.n2.n3.parray = interface_list;

        hr = INetFwRule_put_Interfaces(rule, interfaces);
        ok(hr == S_OK, "Got %08lx\n", hr);

        hr = SafeArrayDestroy(interface_list);
        ok(hr == S_OK, "Got %08lx\n", hr);

        SysFreeString(interface_name);
    }

    SysFreeString(application_name);
    SysFreeString(description);
    SysFreeString(grouping);
    SysFreeString(interface_types);
    SysFreeString(local_addresses);
    SysFreeString(local_ports);
    SysFreeString(name);
    SysFreeString(remote_addresses);
    SysFreeString(remote_ports);
    SysFreeString(service_name);
}

#define verify_rule(rule, rule_info) verify_rule_(rule, rule_info, __LINE__)
static void verify_rule_(INetFwRule *rule, rule_test *rule_info, int line)
{
    HRESULT hr;
    BSTR application_name;
    BSTR description;
    BSTR grouping;
    BSTR interface_types;
    BSTR local_addresses;
    BSTR local_ports;
    BSTR name;
    BSTR remote_addresses;
    BSTR remote_ports;
    BSTR service_name;
    NET_FW_ACTION action;
    NET_FW_RULE_DIRECTION direction;
    VARIANT_BOOL edge_traversal;
    VARIANT_BOOL enabled;
    LONG profiles;
    LONG protocol;
    VARIANT interfaces;

    hr = INetFwRule_get_Action(rule, &action);
    ok_(__FILE__, line)(hr == S_OK, "action: Got %08lx\n", hr);
    hr = INetFwRule_get_ApplicationName(rule, &application_name);
    ok_(__FILE__, line)(hr == S_OK, "application_name: Got %08lx\n", hr);
    hr = INetFwRule_get_Description(rule, &description);
    ok_(__FILE__, line)(hr == S_OK, "description: Got %08lx\n", hr);
    hr = INetFwRule_get_Direction(rule, &direction);
    ok_(__FILE__, line)(hr == S_OK, "direction: Got %08lx\n", hr);
    hr = INetFwRule_get_EdgeTraversal(rule, &edge_traversal);
    ok_(__FILE__, line)(hr == S_OK, "edge_traversal: Got %08lx\n", hr);
    hr = INetFwRule_get_Enabled(rule, &enabled);
    ok_(__FILE__, line)(hr == S_OK, "enabled: Got %08lx\n", hr);
    hr = INetFwRule_get_Grouping(rule, &grouping);
    ok_(__FILE__, line)(hr == S_OK, "grouping: Got %08lx\n", hr);
    hr = INetFwRule_get_Interfaces(rule, &interfaces);
    ok_(__FILE__, line)(hr == S_OK, "interfaces: Got %08lx\n", hr);
    hr = INetFwRule_get_InterfaceTypes(rule, &interface_types);
    ok_(__FILE__, line)(hr == S_OK, "interface_types: Got %08lx\n", hr);
    hr = INetFwRule_get_LocalAddresses(rule, &local_addresses);
    ok_(__FILE__, line)(hr == S_OK, "local_addresses: Got %08lx\n", hr);
    hr = INetFwRule_get_LocalPorts(rule, &local_ports);
    ok_(__FILE__, line)(hr == S_OK, "local_ports: Got %08lx\n", hr);
    hr = INetFwRule_get_Name(rule, &name);
    ok_(__FILE__, line)(hr == S_OK, "name: Got %08lx\n", hr);
    hr = INetFwRule_get_Profiles(rule, &profiles);
    profiles = profiles & 0x7; /* Only lower 3 bits are relevant for this bitmask, so only check those */
    ok_(__FILE__, line)(hr == S_OK, "profiles: Got %08lx\n", hr);
    hr = INetFwRule_get_Protocol(rule, &protocol);
    ok_(__FILE__, line)(hr == S_OK, "protocol: Got %08lx\n", hr);
    hr = INetFwRule_get_RemoteAddresses(rule, &remote_addresses);
    ok_(__FILE__, line)(hr == S_OK, "remote_addresses: Got %08lx\n", hr);
    hr = INetFwRule_get_RemotePorts(rule, &remote_ports);
    ok_(__FILE__, line)(hr == S_OK, "remote_ports: Got %08lx\n", hr);
    hr = INetFwRule_get_ServiceName(rule, &service_name);
    ok_(__FILE__, line)(hr == S_OK, "service_name: Got %08lx\n", hr);

    ok_(__FILE__, line)(rule_info->action == action, "action: Expected %dl, got %dl\n", rule_info->action, action);
    ok_(__FILE__, line)(rule_info->direction == direction, "direction: Expected %d, got %d\n", rule_info->direction, direction);
    ok_(__FILE__, line)(rule_info->profiles == profiles, "profiles: Expected %08lx, got %08lx\n", rule_info->profiles, profiles);
    ok_(__FILE__, line)(rule_info->protocol == protocol, "protocol: Expected %ld, got %ld\n", rule_info->protocol, protocol);
    ok_(__FILE__, line)(rule_info->edge_traversal == edge_traversal, "edge_traversal: Expected %d, got %d\n", rule_info->edge_traversal, edge_traversal);
    ok_(__FILE__, line)(rule_info->enabled == enabled, "enabled: Expected %d, got %d\n", rule_info->enabled, enabled);

    ok_(__FILE__, line)(lstrcmpW(rule_info->application_name, application_name) == 0, "application_name: Expected %s, got %s\n", wine_dbgstr_w(rule_info->application_name), wine_dbgstr_w(application_name));
    ok_(__FILE__, line)(lstrcmpW(rule_info->description, description) == 0, "description: Expected %s, got %s\n", wine_dbgstr_w(rule_info->description), wine_dbgstr_w(description));
    ok_(__FILE__, line)(lstrcmpW(rule_info->grouping, grouping) == 0, "grouping: Expected %s, got %s\n", wine_dbgstr_w(rule_info->grouping), wine_dbgstr_w(grouping));
    ok_(__FILE__, line)(lstrcmpW(rule_info->interface_types, interface_types) == 0, "interface_types: Expected %s, got %s\n", wine_dbgstr_w(rule_info->interface_types), wine_dbgstr_w(interface_types));
    ok_(__FILE__, line)(lstrcmpW(rule_info->local_addresses, local_addresses) == 0, "local_addresses: Expected %s, got %s\n", wine_dbgstr_w(rule_info->local_addresses), wine_dbgstr_w(local_addresses));
    ok_(__FILE__, line)(lstrcmpW(rule_info->local_ports, local_ports) == 0, "local_ports: Expected %s, got %s\n", wine_dbgstr_w(rule_info->local_ports), wine_dbgstr_w(local_ports));
    ok_(__FILE__, line)(lstrcmpW(rule_info->name, name) == 0, "name: Expected %s, got %s\n", wine_dbgstr_w(rule_info->name), wine_dbgstr_w(name));
    ok_(__FILE__, line)(lstrcmpW(rule_info->remote_addresses, remote_addresses) == 0, "remote_addresses: Expected %s, got %s\n", wine_dbgstr_w(rule_info->remote_addresses), wine_dbgstr_w(remote_addresses));
    ok_(__FILE__, line)(lstrcmpW(rule_info->remote_ports, remote_ports) == 0, "remote_ports: Expected %s, got %s\n", wine_dbgstr_w(rule_info->remote_ports), wine_dbgstr_w(remote_ports));
    ok_(__FILE__, line)(lstrcmpW(rule_info->service_name, service_name) == 0, "service_name: Expected %s, got %s\n", wine_dbgstr_w(rule_info->service_name), wine_dbgstr_w(service_name));

    SysFreeString(application_name);
    SysFreeString(description);
    SysFreeString(grouping);
    SysFreeString(interface_types);
    SysFreeString(local_addresses);
    SysFreeString(local_ports);
    SysFreeString(name);
    SysFreeString(remote_addresses);
    SysFreeString(remote_ports);
    SysFreeString(service_name);
}

static void test_INetFwRules(INetFwRules *rules)
{
    rule_test rule_reg;
    INetFwRule *rule_get;
    WCHAR rule_buffer[10000];
    BOOL success;

    static const WCHAR *str_application_name    = L"test-application";
    static const WCHAR *str_description         = L"test-rule-description";
    static const WCHAR *str_grouping            = L"test-grouping";
    static const WCHAR *str_name_empty          = L"wine-test-rule-empty";
    static const WCHAR *str_name_full           = L"wine-test-rule-full";
    static const WCHAR *str_service_name        = L"test-service";
    static const WCHAR *str_interface_types_lan = L"Lan";
    static const WCHAR *str_interface_types_all = L"All";
    static const WCHAR *str_address             = L"127.0.0.0/255.255.255.0";
    static const WCHAR *str_address_all         = L"*";
    static const WCHAR *str_ports               = L"80,443";
    BSTR rule_name_empty = SysAllocString(str_name_empty);
    BSTR rule_name_full = SysAllocString(str_name_full);
    HRESULT hr;
    INetFwRule *rule;
    rule_test rule_info = {0};
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    ULONG len = 0;

    /* Test default values for empty rule */
    hr = CoCreateInstance(&CLSID_NetFwRule, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwRule, (void**)&rule);
    todo_wine
    ok (hr == S_OK, "Got %08lx\n", hr);

    if(!rule)
        goto cleanup;

    hr = INetFwRule_put_Name(rule, rule_name_empty);
    ok(hr == S_OK, "Got %08lx\n", hr);

    hr = INetFwRules_Add(rules, rule);
    if (hr == 0x80070005)
    {
        win_skip("Not enough privileges\n");
        goto cleanup;
    }
    ok (hr == 0, "INetFwRules_Add failed: %08lx\n", hr);

    rule_info.action           = NET_FW_ACTION_ALLOW;
    rule_info.direction        = NET_FW_RULE_DIR_IN;
    rule_info.profiles         = NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_PUBLIC | NET_FW_PROFILE2_DOMAIN;
    rule_info.protocol         = 256; /* Unknown */
    rule_info.interface_types  = str_interface_types_all;
    rule_info.local_addresses  = str_address_all;
    rule_info.name             = str_name_empty;
    rule_info.remote_addresses = str_address_all;

    verify_rule(rule, &rule_info);

    /* Test filled rule */
    hr = CoCreateInstance(&CLSID_NetFwRule, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwRule, (void**)&rule);
    ok(hr == S_OK, "Got %08lx\n", hr);

    if(!rule)
        goto cleanup;

    hr = GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &len);
    ok(hr == ERROR_NO_DATA || hr == ERROR_BUFFER_OVERFLOW, "GetAdaptersAddresses returned %08lx\n", hr);
    if(hr == ERROR_NO_DATA)
    {
        rule_info.interface_name = NULL;
        skip("No adapters found, can't create rule for specific interface\n");
    }
    else
    {
        adapters = heap_alloc(len);
        hr = GetAdaptersAddresses(AF_UNSPEC, 0, NULL, adapters, &len);
        ok(hr == ERROR_SUCCESS, "GetAdaptersAddresses returned %08lx\n", hr);

        rule_info.interface_name = adapters->FriendlyName;
    }

    rule_info.application_name = str_application_name;
    rule_info.description      = str_description;
    rule_info.grouping         = str_grouping;
    rule_info.interface_types  = str_interface_types_lan;
    rule_info.local_addresses  = str_address;
    rule_info.local_ports      = str_ports;
    rule_info.name             = str_name_full;
    rule_info.remote_addresses = str_address;
    rule_info.remote_ports     = str_ports;
    rule_info.service_name     = str_service_name;
    rule_info.protocol         = 6; /* TCP */
    rule_info.action           = NET_FW_ACTION_ALLOW;
    rule_info.enabled          = VARIANT_TRUE;
    rule_info.edge_traversal   = VARIANT_TRUE;
    rule_info.profiles         = NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_PUBLIC | NET_FW_PROFILE2_DOMAIN;
    rule_info.direction        = NET_FW_RULE_DIR_IN;

    fill_rule(rule, &rule_info);
    verify_rule(rule, &rule_info);

    hr = INetFwRules_Add(rules, rule);
    ok (hr == 0, "INetFwRules_Add failed: %08lx\n", hr);

    success = read_rule_from_registry(&rule_reg, str_name_full, rule_buffer, ARRAY_SIZE(rule_buffer));
    ok(success, "Failed to get rule %s\n", wine_dbgstr_w(str_name_full));
    if (success)
        compare_rule(&rule_reg, &rule_info);

    hr = INetFwRules_Item(rules, rule_name_full, &rule_get);
    ok (hr == S_OK, "Got %08lx\n", hr);
    if (hr == S_OK)
        verify_rule(rule_get, &rule_info);

    hr = INetFwRules_Remove(rules, rule_name_full);
    ok (hr == 0, "INetFwRules_Remove failed: %08lx\n", hr);

cleanup:
    SysFreeString(rule_name_empty);
    SysFreeString(rule_name_full);
    if (adapters)
        heap_free(adapters);
}

static void test_policy2_rules(INetFwPolicy2 *policy2)
{
    HRESULT hr;
    INetFwRules *rules, *rules2;
    INetFwServiceRestriction *restriction;

    hr = INetFwPolicy2_QueryInterface(policy2, &IID_INetFwRules, (void**)&rules);
    ok(hr == E_NOINTERFACE, "got 0x%08lx\n", hr);

    hr = INetFwPolicy2_get_Rules(policy2, &rules);
    ok(hr == S_OK, "got %08lx\n", hr);

    hr = INetFwPolicy2_get_Rules(policy2, &rules2);
    ok(hr == S_OK, "got %08lx\n", hr);
    ok(rules == rules2, "Different pointers\n");

    hr = INetFwPolicy2_get_ServiceRestriction(policy2, &restriction);
    todo_wine ok(hr == S_OK, "got %08lx\n", hr);
    if(hr == S_OK)
    {
        INetFwRules *rules3;

        hr = INetFwServiceRestriction_get_Rules(restriction, &rules3);
        ok(hr == S_OK, "got %08lx\n", hr);
        ok(rules != rules3, "same pointers\n");

        if(rules3)
            INetFwRules_Release(rules3);
        INetFwServiceRestriction_Release(restriction);
    }

    hr = INetFwRules_get__NewEnum(rules, NULL);
    ok(hr == E_POINTER, "got %08lx\n", hr);

    test_INetFwRules(rules);

    INetFwRules_Release(rules);
    INetFwRules_Release(rules2);
}

static void test_interfaces(void)
{
    INetFwMgr *manager;
    INetFwPolicy *policy;
    INetFwPolicy2 *policy2;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_NetFwMgr, NULL, CLSCTX_INPROC_SERVER|CLSCTX_INPROC_HANDLER,
            &IID_INetFwMgr, (void**)&manager);
    ok(hr == S_OK, "NetFwMgr create failed: %08lx\n", hr);

    hr = INetFwMgr_QueryInterface(manager, &IID_INetFwPolicy, (void**)&policy);
    ok(hr == E_NOINTERFACE, "got 0x%08lx\n", hr);

    hr = INetFwMgr_QueryInterface(manager, &IID_INetFwPolicy2, (void**)&policy2);
    ok(hr == E_NOINTERFACE, "got 0x%08lx\n", hr);

    hr = INetFwMgr_get_LocalPolicy(manager, &policy);
    ok(hr == S_OK, "got 0x%08lx\n", hr);

    hr = INetFwPolicy_QueryInterface(policy, &IID_INetFwPolicy2, (void**)&policy2);
    ok(hr == E_NOINTERFACE, "got 0x%08lx\n", hr);

    INetFwPolicy_Release(policy);

    hr = CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER|CLSCTX_INPROC_HANDLER,
            &IID_INetFwPolicy2, (void**)&policy2);
    if(hr == S_OK)
    {
        test_policy2_rules(policy2);

        INetFwPolicy2_Release(policy2);
    }
    else
        win_skip("NetFwPolicy2 object is not supported: %08lx\n", hr);

    INetFwMgr_Release(manager);
}

static void test_NetFwAuthorizedApplication(void)
{
    INetFwAuthorizedApplication *app;
    static WCHAR empty[] = L"";
    UNIVERSAL_NAME_INFOW *info;
    WCHAR fullpath[MAX_PATH];
    WCHAR netpath[MAX_PATH];
    WCHAR image[MAX_PATH];
    HRESULT hr;
    BSTR bstr;
    DWORD sz;

    hr = CoCreateInstance(&CLSID_NetFwAuthorizedApplication, NULL, CLSCTX_INPROC_SERVER|CLSCTX_INPROC_HANDLER,
            &IID_INetFwAuthorizedApplication, (void**)&app);
    ok(hr == S_OK, "got: %08lx\n", hr);

    hr = GetModuleFileNameW(NULL, image, ARRAY_SIZE(image));
    ok(hr, "GetModuleFileName failed: %lu\n", GetLastError());

    hr = INetFwAuthorizedApplication_get_ProcessImageFileName(app, NULL);
    ok(hr == E_POINTER, "got: %08lx\n", hr);

    hr = INetFwAuthorizedApplication_get_ProcessImageFileName(app, &bstr);
    ok(hr == S_OK || hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY), "got: %08lx\n", hr);
    ok(!bstr, "got: %s\n",  wine_dbgstr_w(bstr));

    hr = INetFwAuthorizedApplication_put_ProcessImageFileName(app, NULL);
    ok(hr == E_INVALIDARG || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), "got: %08lx\n", hr);

    hr = INetFwAuthorizedApplication_put_ProcessImageFileName(app, empty);
    ok(hr == E_INVALIDARG || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), "got: %08lx\n", hr);

    bstr = SysAllocString(image);
    hr = INetFwAuthorizedApplication_put_ProcessImageFileName(app, bstr);
    ok(hr == S_OK, "got: %08lx\n", hr);
    SysFreeString(bstr);

    GetFullPathNameW(image, ARRAY_SIZE(fullpath), fullpath, NULL);
    GetLongPathNameW(fullpath, fullpath, ARRAY_SIZE(fullpath));

    info = (UNIVERSAL_NAME_INFOW *)&netpath;
    sz = sizeof(netpath);
    hr = WNetGetUniversalNameW(image, UNIVERSAL_NAME_INFO_LEVEL, info, &sz);
    if (hr != NO_ERROR)
    {
        info->lpUniversalName = netpath + sizeof(*info)/sizeof(WCHAR);
        lstrcpyW(info->lpUniversalName, fullpath);
    }

    hr = INetFwAuthorizedApplication_get_ProcessImageFileName(app, &bstr);
    ok(hr == S_OK, "got: %08lx\n", hr);
    ok(!lstrcmpW(bstr,info->lpUniversalName), "expected %s, got %s\n",
        wine_dbgstr_w(info->lpUniversalName), wine_dbgstr_w(bstr));
    SysFreeString(bstr);

    INetFwAuthorizedApplication_Release(app);
}

static void test_static_port_mapping_collection( IStaticPortMappingCollection *ports )
{
    LONG i, count, count2, expected_count, external_port;
    IStaticPortMapping *pm, *pm2;
    ULONG refcount, refcount2;
    IEnumVARIANT *enum_ports;
    IUnknown *unk;
    ULONG fetched;
    BSTR protocol;
    VARIANT var;
    HRESULT hr;

    refcount = get_refcount((IUnknown *)ports);
    hr = IStaticPortMappingCollection_get__NewEnum(ports, &unk);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IUnknown_QueryInterface(unk, &IID_IEnumVARIANT, (void **)&enum_ports);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IUnknown_Release( unk );

    refcount2 = get_refcount((IUnknown *)ports);
    ok(refcount2 == refcount, "Got unexpected refcount %lu, refcount2 %lu.\n", refcount, refcount2);

    hr = IEnumVARIANT_Reset(enum_ports);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    count = 0xdeadbeef;
    hr = IStaticPortMappingCollection_get_Count(ports, &count);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IStaticPortMappingCollection_get_Item(ports, 12345, (BSTR)L"UDP", &pm);
    if (SUCCEEDED(hr))
    {
        expected_count = count;
        IStaticPortMapping_Release(pm);
    }
    else
    {
        expected_count = count + 1;
    }

    hr = IStaticPortMappingCollection_Add(ports, 12345, (BSTR)L"udp", 12345, (BSTR)L"1.2.3.4",
            VARIANT_TRUE, (BSTR)L"wine_test", &pm);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = IStaticPortMappingCollection_Add(ports, 12345, (BSTR)L"UDP", 12345, (BSTR)L"1.2.3.4",
            VARIANT_TRUE, (BSTR)L"wine_test", &pm);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IStaticPortMappingCollection_get_Count(ports, &count2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(count2 == expected_count, "Got unexpected count2 %lu, expected %lu.\n", count2, expected_count);

    hr = IStaticPortMappingCollection_get_Item(ports, 12345, NULL, &pm);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IStaticPortMappingCollection_get_Item(ports, 12345, (BSTR)L"UDP", NULL);
    ok(hr == E_POINTER, "Got unexpected hr %#lx.\n", hr);

    hr = IStaticPortMappingCollection_get_Item(ports, 12345, (BSTR)L"udp", &pm);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IStaticPortMappingCollection_get_Item(ports, -1, (BSTR)L"UDP", &pm);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IStaticPortMappingCollection_get_Item(ports, 65536, (BSTR)L"UDP", &pm);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IStaticPortMappingCollection_get_Item(ports, 12346, (BSTR)L"UDP", &pm);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "Got unexpected hr %#lx.\n", hr);

    hr = IEnumVARIANT_Reset(enum_ports);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    for (i = 0; i < count2; ++i)
    {
        VariantInit(&var);

        fetched = 0xdeadbeef;
        hr = IEnumVARIANT_Next(enum_ports, 1, &var, &fetched);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        ok(fetched == 1, "Got unexpected fetched %lu.\n", fetched);
        ok(V_VT(&var) == VT_DISPATCH, "Got unexpected variant type %u.\n", V_VT(&var));

        hr = IDispatch_QueryInterface(V_DISPATCH(&var), &IID_IStaticPortMapping, (void **)&pm);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        hr = IStaticPortMapping_get_Protocol(pm, &protocol);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        external_port = 0xdeadbeef;
        hr = IStaticPortMapping_get_ExternalPort(pm, &external_port);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        ok(!wcscmp(protocol, L"UDP") || !wcscmp(protocol, L"TCP"), "Got unexpected protocol %s.\n",
                debugstr_w(protocol));
        hr = IStaticPortMappingCollection_get_Item(ports, external_port, protocol, &pm2);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        ok(pm2 != pm, "Got same interface.\n");

        IStaticPortMapping_Release(pm);
        IStaticPortMapping_Release(pm2);

        SysFreeString(protocol);

        VariantClear(&var);
    }
    hr = IEnumVARIANT_Next(enum_ports, 1, &var, &fetched);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);

    hr = IStaticPortMappingCollection_Remove(ports, 12345, (BSTR)L"UDP");
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IEnumVARIANT_Release(enum_ports);
}

static void test_IUPnPNAT(void)
{
    IUPnPNAT *nat;
    IStaticPortMappingCollection *static_ports;
    IDynamicPortMappingCollection *dync_ports;
    INATEventManager *manager;
    IProvideClassInfo *provider;
    ULONG refcount, refcount2;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_UPnPNAT, NULL, CLSCTX_INPROC_SERVER|CLSCTX_INPROC_HANDLER, &IID_IUPnPNAT, (void**)&nat);
    ok(hr == S_OK, "got: %08lx\n", hr);

    hr = IUPnPNAT_QueryInterface(nat, &IID_IProvideClassInfo, (void**)&provider);
    ok(hr == E_NOINTERFACE, "got: %08lx\n", hr);

    refcount = get_refcount((IUnknown *)nat);
    hr = IUPnPNAT_get_StaticPortMappingCollection(nat, &static_ports);

    ok(hr == S_OK, "got: %08lx\n", hr);
    if(hr == S_OK && static_ports)
    {
        refcount2 = get_refcount((IUnknown *)nat);
        ok(refcount2 == refcount, "Got unexpected refcount %lu, refcount2 %lu.\n", refcount, refcount2);
        test_static_port_mapping_collection( static_ports );
        IStaticPortMappingCollection_Release(static_ports);
    }
    else if (hr == S_OK)
    {
        skip( "UPNP gateway not found.\n" );
    }
    hr = IUPnPNAT_get_DynamicPortMappingCollection(nat, &dync_ports);
    ok(hr == S_OK || hr == E_NOTIMPL /* Windows 8.1 */, "got: %08lx\n", hr);
    if(hr == S_OK && dync_ports)
        IDynamicPortMappingCollection_Release(dync_ports);

    hr = IUPnPNAT_get_NATEventManager(nat, &manager);
    todo_wine ok(hr == S_OK, "got: %08lx\n", hr);
    if(hr == S_OK && manager)
        INATEventManager_Release(manager);

    IUPnPNAT_Release(nat);
}


START_TEST(policy)
{
    INetFwMgr *manager;
    HRESULT hr;

    CoInitialize(NULL);

    hr = CoCreateInstance(&CLSID_NetFwMgr, NULL, CLSCTX_INPROC_SERVER|CLSCTX_INPROC_HANDLER,
            &IID_INetFwMgr, (void**)&manager);
    if(FAILED(hr))
    {
        win_skip("NetFwMgr object is not supported: %08lx\n", hr);
        CoUninitialize();
        return;
    }

    INetFwMgr_Release(manager);

    test_interfaces();
    test_NetFwAuthorizedApplication();
    test_IUPnPNAT();

    CoUninitialize();
}
