/*
 * Copyright 2024-2025 Zhiyi Zhang for CodeWeavers
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
#include <winternl.h>
#include <inaddr.h>
#include <in6addr.h>
#include <ip2string.h>
#include <wchar.h>

WINE_DEFAULT_DEBUG_CHANNEL(iertutil);

#define RESERVED_SUB_DELIMITER_CHARS    L"!$&'()*+,;="
#define ALLOWED_USER_RESERVED_CHARS     RESERVED_SUB_DELIMITER_CHARS L":"
#define ALLOWED_PASSWORD_RESERVED_CHARS RESERVED_SUB_DELIMITER_CHARS L":"
#define ALLOWED_HOST_RESERVED_CHARS     RESERVED_SUB_DELIMITER_CHARS
#define ALLOWED_PATH_RESERVED_CHARS     RESERVED_SUB_DELIMITER_CHARS L"/:@"

struct uri
{
    IUriRuntimeClass IUriRuntimeClass_iface;
    HSTRING absolute_uri;
    HSTRING display_uri;
    HSTRING extension;
    HSTRING fragment;
    HSTRING host;
    HSTRING password;
    HSTRING path;
    HSTRING query;
    HSTRING scheme;
    HSTRING raw_uri;
    HSTRING user;
    INT32 port;
    BOOL is_host_ipv6;
    BOOL has_authority;
    BOOL has_user_semicolon;
    LONG ref;
};

static const struct known_scheme
{
    const WCHAR *scheme;
    int default_port;
}
known_schemes[] =
{
    {L"file", 0},
    {L"ftp", 21},
    {L"telnet", 23},
    {L"http", 80},
    {L"https", 443},
};

static WCHAR *strdupwn(const WCHAR *str, DWORD length)
{
    WCHAR *ret;

    if (!str)
        return NULL;

    if ((ret = malloc((length + 1) * sizeof(WCHAR))))
    {
        memcpy(ret, str, length * sizeof(WCHAR));
        ret[length] = '\0';
    }
    return ret;
}

static BOOL is_char_valid(WCHAR ch, const WCHAR *allowed_reserved_chars)
{
    if (iswalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~')
        return TRUE;
    if (allowed_reserved_chars && wcschr(allowed_reserved_chars, ch))
        return TRUE;
    return FALSE;
}

static BOOL is_known_scheme(HSTRING scheme)
{
    const WCHAR *buffer;
    unsigned int i;

    buffer = WindowsGetStringRawBuffer(scheme, NULL);
    for (i = 0; i < ARRAY_SIZE(known_schemes); i++)
    {
        if (!wcsicmp(buffer, known_schemes[i].scheme))
            return TRUE;
    }
    return FALSE;
}

static INT32 get_known_scheme_default_port(HSTRING scheme)
{
    const WCHAR *buffer;
    unsigned int i;

    buffer = WindowsGetStringRawBuffer(scheme, NULL);
    for (i = 0; i < ARRAY_SIZE(known_schemes); i++)
    {
        if (!wcsicmp(buffer, known_schemes[i].scheme))
            return known_schemes[i].default_port;
    }
    return 0;
}

static HRESULT percent_encode(HSTRING scheme, const WCHAR *start, unsigned length,
                              const WCHAR *allowed_reserved_chars, WCHAR **result)
{
    static const WCHAR *hex = L"0123456789ABCDEF";
    const WCHAR *p = start, *end = start + length;
    unsigned int buffer_length = 0;
    WCHAR *buffer;

    /* If not known scheme, don't percent encode. Simply make a NUL-terminated string */
    if (!is_known_scheme(scheme))
    {
        buffer = strdupwn(start, length);
        if (!buffer)
            return E_OUTOFMEMORY;

        *result = buffer;
        return S_OK;
    }

    /* Percent encode */
    buffer = malloc((length * 3 + 1) * sizeof(WCHAR));
    if (!buffer)
        return E_OUTOFMEMORY;

    while (p < end)
    {
        /* Unicode characters or allowed characters */
        if (*p >= 0x80 || is_char_valid(*p, allowed_reserved_chars))
        {
            buffer[buffer_length++] = *p;
            p++;
        }
        /* Already percent encoded. Copy them over */
        else if (*p == '%')
        {
            if (p + 2 >= end || !iswxdigit(p[1]) || !iswxdigit(p[2]))
            {
                free(buffer);
                return E_INVALIDARG;
            }

            memcpy(&buffer[buffer_length], p, 3 * sizeof(WCHAR));
            buffer_length += 3;
            p += 3;
        }
        /* Percent encode */
        else
        {
            buffer[buffer_length++] = '%';
            buffer[buffer_length++] = hex[*p >> 4];
            buffer[buffer_length++] = hex[*p & 0xf];
            p++;
        }
    }

    buffer[buffer_length] = '\0';
    *result = buffer;
    return S_OK;
}

static HRESULT parse_user_info(struct uri *uri, const WCHAR *start, unsigned int length)
{
    const WCHAR *user_end = NULL, *password_end = NULL;
    WCHAR *user, *password;
    HRESULT hr;

    password_end = start + length;
    user_end = wmemchr(start, ':', password_end - start);
    if (!user_end)
        user_end = password_end;

    if (FAILED(hr = percent_encode(uri->scheme, start, user_end - start,
                                   ALLOWED_USER_RESERVED_CHARS, &user)))
        return hr;
    WindowsCreateString(user, wcslen(user), &uri->user);
    free(user);

    if (*user_end == ':')
    {
        uri->has_user_semicolon = TRUE;
        start = user_end + 1;

        if (FAILED(hr = percent_encode(uri->scheme, start, password_end - start,
                                       ALLOWED_PASSWORD_RESERVED_CHARS, &password)))
            return hr;
        WindowsCreateString(password, wcslen(password), &uri->password);
        free(password);
    }
    return S_OK;
}

static HRESULT parse_ipv6_host(struct uri *uri, const WCHAR *start, unsigned int length)
{
    HRESULT hr = E_INVALIDARG;
    const WCHAR *terminator;
    WCHAR *addr = NULL;
    IN6_ADDR in6_addr;

    if (start[length - 1] != ']')
        goto failed;

    /* Zone ID is not allowed */
    if (wcschr(start, '%'))
        goto failed;

    /* Strip [] */
    addr = strdupwn(start + 1, length - 2);
    if (!addr)
        return E_OUTOFMEMORY;

    if (RtlIpv6StringToAddressW(addr, &terminator, &in6_addr))
        goto failed;

    uri->is_host_ipv6 = TRUE;
    WindowsCreateString(addr, wcslen(addr), &uri->host);
    hr = S_OK;

failed:
    free(addr);
    return hr;
}

static HRESULT parse_ipv4_host(struct uri *uri, const WCHAR *start, unsigned int length)
{
    WCHAR *addr = NULL, ipv4_string[16];
    const WCHAR *terminator;
    IN_ADDR in_addr;

    addr = strdupwn(start, length);
    if (!addr)
        return E_OUTOFMEMORY;

    if (!RtlIpv4StringToAddressW(addr, FALSE, &terminator, &in_addr))
    {
        free(addr);
        RtlIpv4AddressToStringW(&in_addr, ipv4_string);
        WindowsCreateString(ipv4_string, wcslen(ipv4_string), &uri->host);
        return S_OK;
    }

    free(addr);
    return E_INVALIDARG;
}

static HRESULT parse_host(struct uri *uri, const WCHAR *start, unsigned int length)
{
    WCHAR *host = NULL, *unicode_host = NULL;
    int unicode_host_length, ret;
    HRESULT hr;

    if (*start == '[')
    {
        if (FAILED(hr = parse_ipv6_host(uri, start, length)))
        {
            WARN("Failed to parse ipv6 host %s.\n", wine_dbgstr_wn(start, length));
            return hr;
        }
        return S_OK;
    }

    if (iswdigit(*start))
    {
        if (FAILED(hr = parse_ipv4_host(uri, start, length)))
        {
            WARN("Failed to parse ipv4 host %s.\n", wine_dbgstr_wn(start, length));
            return hr;
        }
        return S_OK;
    }

    if (FAILED(hr = percent_encode(uri->scheme, start, length, ALLOWED_HOST_RESERVED_CHARS, &host)))
    {
        WARN("Failed to encode host %s.\n", wine_dbgstr_wn(start, length));
        return E_FAIL;
    }

    CharLowerBuffW(host, wcslen(host));

    unicode_host_length = IdnToUnicode(0, host, wcslen(host), NULL, 0);
    unicode_host = malloc(unicode_host_length * sizeof(WCHAR));
    ret = IdnToUnicode(0, host, wcslen(host), unicode_host, unicode_host_length);
    if (!ret)
    {
        WARN("Failed to convert host %s from IDN to Unicode.\n", wine_dbgstr_w(host));
        free(unicode_host);
        free(host);
        return E_FAIL;
    }

    WindowsCreateString(unicode_host, unicode_host_length, &uri->host);
    free(unicode_host);
    free(host);
    return S_OK;
}

static HRESULT parse_port(struct uri *uri, const WCHAR *start, unsigned int length)
{
    WCHAR *end;
    int port;

    /* wcstol() allows + and - */
    if (!iswdigit(*start))
    {
        WARN("Could not parse port %s.\n", wine_dbgstr_wn(start, length));
        return E_INVALIDARG;
    }

    port = wcstol(start, &end, 10);
    if (end != start + length)
    {
        WARN("Could not parse port %s.\n", wine_dbgstr_wn(start, length));
        return E_INVALIDARG;
    }
    else if (port > 65535)
    {
        WARN("Port %s is out of range.\n", wine_dbgstr_wn(start, length));
        return E_INVALIDARG;
    }

    uri->port = port;
    return S_OK;
}

static void parse_extension(struct uri *uri, const WCHAR *start, unsigned int length)
{
    const WCHAR *last_point = NULL, *p = start;

    while (*p)
    {
        if (*p == '\\' || *p == ' ')
            last_point = NULL;
        else if (*p == '.')
            last_point = p;
        p++;
    }

    if (last_point)
        WindowsCreateString(last_point, length - (last_point - start), &uri->extension);
}

static void remove_dot_segments(WCHAR *path)
{
    WCHAR *src = path, *dst = path;

    if (!*path)
        return;

    while (*src)
    {
        if (!wcsncmp(src, L"../", 3))
        {
            src += 3;
        }
        else if (!wcsncmp(src, L"./", 2))
        {
            src += 2;
        }
        else if (!wcsncmp(src, L"/./", 3))
        {
            src += 2;
        }
        else if (!wcscmp(src, L"/."))
        {
            src[1] = '\0';
        }
        else if (!wcsncmp(src, L"/../", 4))
        {
            src += 3;
            if (dst > path)
            {
                do
                {
                    dst--;
                } while (*dst != '/' && dst > path);
            }
        }
        else if (!wcscmp(src, L"/.."))
        {
            src[1] = '\0';
            if (dst > path)
            {
                do
                {
                    dst--;
                } while (*dst != '/' && dst > path);
            }
        }
        else if (!wcscmp(src, L"..") || !wcscmp(src, L"."))
        {
            src[0] = '\0';
        }
        else
        {
            *dst++ = *src++;
            while (*src && *src != '/')
                *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void normalize_backslashes(WCHAR *start, unsigned int length)
{
    unsigned int i = 0;

    for (i = 0; i < length; i++)
    {
        if (start[i] == '?' || start[i] == '#')
            break;

        if (start[i] == '\\')
            start[i] = '/';
    }
}

static void construct_uri_string(struct uri *uri, BOOL check_authority, BOOL include_user_info, HSTRING *result)
{
    unsigned int scheme_length, user_length, password_length, host_length, path_length, query_length, fragment_length, length = 0;
    const WCHAR *scheme, *user, *password, *host, *path, *query, *fragment;
    WCHAR *buffer, port[6];

    /* Display URI doesn't check authority */
    if (check_authority && is_known_scheme(uri->scheme) && !uri->has_authority)
        return;

    scheme = WindowsGetStringRawBuffer(uri->scheme, &scheme_length);
    user = WindowsGetStringRawBuffer(uri->user, &user_length);
    password = WindowsGetStringRawBuffer(uri->password, &password_length);
    host = WindowsGetStringRawBuffer(uri->host, &host_length);
    path = WindowsGetStringRawBuffer(uri->path, &path_length);
    query = WindowsGetStringRawBuffer(uri->query, &query_length);
    fragment = WindowsGetStringRawBuffer(uri->fragment, &fragment_length);

    length = scheme_length + 3 /* :// */ + user_length + 1 /* : */ + password_length
             + 1 /* @ */ + host_length + 2 /* [] for IPv6 */ + 6 /* :port(max 65535) */
             + path_length + query_length + fragment_length;
    buffer = malloc((length + 1) * sizeof(WCHAR));
    if (!buffer)
        return;

    wcscpy(buffer, scheme);

    if (uri->has_authority)
        wcscat(buffer, L"://");
    else
        wcscat(buffer, L":");

    if (include_user_info)
    {
        wcscat(buffer, user);

        if (uri->has_user_semicolon && (user_length || password_length))
            wcscat(buffer, L":");

        wcscat(buffer, password);

        if (user_length || password_length)
            wcscat(buffer, L"@");
    }

    if (uri->is_host_ipv6)
        wcscat(buffer, L"[");
    wcscat(buffer, host);
    if (uri->is_host_ipv6)
        wcscat(buffer, L"]");

    if (uri->port && uri->port != get_known_scheme_default_port(uri->scheme))
    {
        wcscat(buffer, L":");
        swprintf_s(port, ARRAY_SIZE(port) - 1, L"%d", uri->port);
        wcscat(buffer, port);
    }

    wcscat(buffer, path);
    wcscat(buffer, query);
    wcscat(buffer, fragment);

    WindowsCreateString(buffer, wcslen(buffer), result);
    free(buffer);
}

static WCHAR *get_raw_uri(const WCHAR *start, unsigned int length)
{
    WCHAR *buffer, *p;
    unsigned int i;

    buffer = malloc((length + 1) * sizeof(WCHAR));
    if (!buffer)
        return NULL;

    /* White spaces other than ' ' are not allowed in the raw URI */
    for (i = 0, p = buffer; i < length; i++)
    {
        if (start[i] == ' ' || !iswspace(start[i]))
        {
            *p = start[i];
            p++;
        }
    }
    *p = '\0';
    return buffer;
}

static WCHAR *get_full_uri(WCHAR *uri, unsigned int length)
{
    static const WCHAR file_scheme[] = L"file:///";
    WCHAR *buffer;

    /* add the implicit file scheme if it matches X: pattern*/
    if (length >= 2 && iswalpha(uri[0]) && uri[1] == ':')
    {
        buffer = malloc(sizeof(file_scheme) + length * sizeof(WCHAR));
        if (!buffer)
            return NULL;

        wcscpy(buffer, file_scheme);
        wcscat(buffer, uri);
        return buffer;
    }
    else
    {
        return strdupwn(uri, length);
    }
}

static HRESULT parse_uri_string(HSTRING string, struct uri *uri)
{
    const WCHAR *end, *colon, *at, *path_start, *query, *bracket, *host_end, *p, *raw_buffer;
    WCHAR *uri_string, *buffer, *path, *raw_uri;
    UINT32 length;
    HRESULT hr;

    raw_buffer = WindowsGetStringRawBuffer(string, &length);
    raw_uri = get_raw_uri(raw_buffer, length);
    if (!raw_uri)
        return E_OUTOFMEMORY;

    uri_string = get_full_uri(raw_uri, wcslen(raw_uri));
    if (!uri_string)
        return E_OUTOFMEMORY;

    p = uri_string;
    length = wcslen(uri_string);

    /* Parse scheme */
    while (*p && (iswalpha(*p) || (iswdigit(*p) || *p == '.' || *p == '+' || *p == '-')))
        p++;

    if (p > uri_string && *p == ':')
    {
        length = p - uri_string;
        buffer = strdupwn(uri_string, length);
        if (!buffer)
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }

        CharLowerBuffW(buffer, length);
        WindowsCreateString(buffer, length, &uri->scheme);
        free(buffer);
        p++;
    }
    else
    {
        WARN("No scheme is found in %s.\n", wine_dbgstr_hstring(string));
        return E_INVALIDARG;
    }

    if (is_known_scheme(uri->scheme))
        normalize_backslashes(uri_string, wcslen(uri_string));

    /* Parse authority */
    if (!wcsncmp(p, L"//", 2))
    {
        uri->has_authority = TRUE;

        p += 2;
        path_start = p + wcscspn(p, L"/?#");
        at = wmemchr(p, L'@', path_start - p);
        if (at)
        {
            if (FAILED(hr = parse_user_info(uri, p, at - p)))
            {
                WARN("Failed to parse userinfo %s.\n", wine_dbgstr_wn(p, at - p));
                goto done;
            }

            p = at + 1;
        }

        /* Parse host */
        if (*p == '[')
        {
            bracket = wmemchr(p, ']', path_start - p);
            if (bracket && *(bracket + 1) == ':')
                colon = bracket + 1;
            else
                colon = NULL;
        }
        else
        {
            colon = wmemchr(p, ':', path_start - p);
        }

        host_end = colon ? colon : path_start;
        if (*p && host_end > p && FAILED(hr = parse_host(uri, p, host_end - p)))
            goto done;

        /* Parse port */
        if (colon && colon != path_start - 1)
        {
            p = colon + 1;
            if (FAILED(hr = parse_port(uri, p, path_start - p)))
                goto done;
        }

        p = path_start;
    }

    /* Parse fragment */
    end = p + wcscspn(p, L"#");
    if (*end == '#')
        WindowsCreateString(end, wcslen(end), &uri->fragment);

    /* Parse query */
    query = wmemchr(p, '?', end - p);
    if (query)
    {
        WindowsCreateString(query, end - query, &uri->query);
        end = query;
    }

    /* Parse path */
    if (FAILED(hr = percent_encode(uri->scheme, p, end - p, ALLOWED_PATH_RESERVED_CHARS, &path)))
    {
        WARN("Failed to percent encode path %s.\n", wine_dbgstr_w(query + 1));
        goto done;
    }

    remove_dot_segments(path);

    if (uri->has_authority && path && path[0] == '\0')
    {
        free(path);
        path = strdupwn(L"/", 1);
        if (!path)
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
    }

    WindowsCreateString(path, wcslen(path), &uri->path);

    /* Parse extension */
    parse_extension(uri, path, wcslen(path));
    free(path);

    /* Get the default port */
    if (!uri->port)
        uri->port = get_known_scheme_default_port(uri->scheme);

    construct_uri_string(uri, TRUE, TRUE, &uri->absolute_uri);
    construct_uri_string(uri, FALSE, !is_known_scheme(uri->scheme), &uri->display_uri);
    WindowsCreateString(raw_uri, wcslen(raw_uri), &uri->raw_uri);
    hr = S_OK;

done:
    if (FAILED(hr))
    {
        WindowsDeleteString(uri->absolute_uri);
        WindowsDeleteString(uri->display_uri);
        WindowsDeleteString(uri->extension);
        WindowsDeleteString(uri->fragment);
        WindowsDeleteString(uri->host);
        WindowsDeleteString(uri->password);
        WindowsDeleteString(uri->path);
        WindowsDeleteString(uri->query);
        WindowsDeleteString(uri->scheme);
        WindowsDeleteString(uri->raw_uri);
        WindowsDeleteString(uri->user);
        free(uri_string);
        free(raw_uri);
    }
    return hr;
}

static inline struct uri *impl_from_IUriRuntimeClass(IUriRuntimeClass *iface)
{
    return CONTAINING_RECORD(iface, struct uri, IUriRuntimeClass_iface);
}

static HRESULT STDMETHODCALLTYPE uri_QueryInterface(IUriRuntimeClass *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p stub!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
        || IsEqualGUID(iid, &IID_IInspectable)
        || IsEqualGUID(iid, &IID_IUriRuntimeClass))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE uri_AddRef(IUriRuntimeClass *iface)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE uri_Release(IUriRuntimeClass *iface)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        WindowsDeleteString(impl->absolute_uri);
        WindowsDeleteString(impl->display_uri);
        WindowsDeleteString(impl->extension);
        WindowsDeleteString(impl->fragment);
        WindowsDeleteString(impl->host);
        WindowsDeleteString(impl->password);
        WindowsDeleteString(impl->path);
        WindowsDeleteString(impl->query);
        WindowsDeleteString(impl->scheme);
        WindowsDeleteString(impl->raw_uri);
        WindowsDeleteString(impl->user);
        free(impl);
    }

    return ref;
}

static HRESULT STDMETHODCALLTYPE uri_GetIids(IUriRuntimeClass *iface, ULONG *iid_count,
        IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE uri_GetRuntimeClassName(IUriRuntimeClass *iface,
                                                         HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE uri_GetTrustLevel(IUriRuntimeClass *iface, TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE uri_AbsoluteUri(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->absolute_uri, value);
}

static HRESULT STDMETHODCALLTYPE uri_DisplayUri(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->display_uri, value);
}

static HRESULT STDMETHODCALLTYPE uri_Domain(IUriRuntimeClass *iface, HSTRING *value)
{
    FIXME("iface %p, value %p stub!\n", iface, value);

    /* TODO: Needs to use things like psl_registrable_domain() in libpsl to get the domain from the host name */

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE uri_Extension(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->extension, value);
}

static HRESULT STDMETHODCALLTYPE uri_Fragment(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->fragment, value);
}

static HRESULT STDMETHODCALLTYPE uri_Host(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->host, value);
}

static HRESULT STDMETHODCALLTYPE uri_Password(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->password, value);
}

static HRESULT STDMETHODCALLTYPE uri_Path(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->path, value);
}

static HRESULT STDMETHODCALLTYPE uri_Query(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->query, value);
}

static HRESULT STDMETHODCALLTYPE uri_QueryParsed(IUriRuntimeClass *iface,
                                                 IWwwFormUrlDecoderRuntimeClass **decoder)
{
    FIXME("iface %p, decoder %p stub!\n", iface, decoder);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE uri_RawUri(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->raw_uri, value);
}

static HRESULT STDMETHODCALLTYPE uri_SchemeName(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->scheme, value);
}

static HRESULT STDMETHODCALLTYPE uri_UserName(IUriRuntimeClass *iface, HSTRING *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->user, value);
}

static HRESULT STDMETHODCALLTYPE uri_Port(IUriRuntimeClass *iface, INT32 *value)
{
    struct uri *impl = impl_from_IUriRuntimeClass(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    if (!impl->port)
        return S_FALSE;

    *value = impl->port;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE uri_Suspicious(IUriRuntimeClass *iface, boolean *value)
{
    FIXME("iface %p, value %p stub!\n", iface, value);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE uri_Equals(IUriRuntimeClass *iface, IUriRuntimeClass *uri,
                                            boolean *value)
{
    FIXME("iface %p, uri %p, value %p stub!\n", iface, uri, value);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE uri_CombineUri(IUriRuntimeClass *iface, HSTRING relative_uri,
                                                IUriRuntimeClass **instance)
{
    FIXME("iface %p, relative_uri %s, instance %p stub!\n", iface, debugstr_hstring(relative_uri), instance);
    return E_NOTIMPL;
}

static const struct IUriRuntimeClassVtbl uri_vtbl =
{
    uri_QueryInterface,
    uri_AddRef,
    uri_Release,
    /* IInspectable methods */
    uri_GetIids,
    uri_GetRuntimeClassName,
    uri_GetTrustLevel,
    /* IUriRuntimeClass methods */
    uri_AbsoluteUri,
    uri_DisplayUri,
    uri_Domain,
    uri_Extension,
    uri_Fragment,
    uri_Host,
    uri_Password,
    uri_Path,
    uri_Query,
    uri_QueryParsed,
    uri_RawUri,
    uri_SchemeName,
    uri_UserName,
    uri_Port,
    uri_Suspicious,
    uri_Equals,
    uri_CombineUri,
};

struct iertutil
{
    IActivationFactory IActivationFactory_iface;
    IUriRuntimeClassFactory IUriRuntimeClassFactory_iface;
    LONG ref;
};

static inline struct iertutil *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct iertutil, IActivationFactory_iface);
}

static HRESULT STDMETHODCALLTYPE iertutil_QueryInterface(IActivationFactory *iface, REFIID iid,
                                                         void **out)
{
    struct iertutil *impl = impl_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p stub!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
        || IsEqualGUID(iid, &IID_IInspectable)
        || IsEqualGUID(iid, &IID_IAgileObject)
        || IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IUriRuntimeClassFactory))
    {
        IUriRuntimeClassFactory_AddRef(&impl->IUriRuntimeClassFactory_iface);
        *out = &impl->IUriRuntimeClassFactory_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE iertutil_AddRef(IActivationFactory *iface)
{
    struct iertutil *impl = impl_from_IActivationFactory(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE iertutil_Release(IActivationFactory *iface)
{
    struct iertutil *impl = impl_from_IActivationFactory(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static HRESULT STDMETHODCALLTYPE iertutil_GetIids(IActivationFactory *iface, ULONG *iid_count,
                                                  IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE iertutil_GetRuntimeClassName(IActivationFactory *iface,
                                                              HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE iertutil_GetTrustLevel(IActivationFactory *iface,
                                                        TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE iertutil_ActivateInstance(IActivationFactory *iface,
                                                           IInspectable **instance)
{
    FIXME("iface %p, instance %p stub!\n", iface, instance);
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl activation_factory_vtbl =
{
    iertutil_QueryInterface,
    iertutil_AddRef,
    iertutil_Release,
    /* IInspectable methods */
    iertutil_GetIids,
    iertutil_GetRuntimeClassName,
    iertutil_GetTrustLevel,
    /* IActivationFactory methods */
    iertutil_ActivateInstance,
};

DEFINE_IINSPECTABLE(uri_factory, IUriRuntimeClassFactory, struct iertutil, IActivationFactory_iface)

static HRESULT STDMETHODCALLTYPE uri_factory_CreateUri(IUriRuntimeClassFactory *iface, HSTRING string,
                                                       IUriRuntimeClass **instance)
{
    struct uri *uri;
    HRESULT hr;

    TRACE("iface %p, uri %s, instance %p.\n", iface, debugstr_hstring(string), instance);

    if (!string)
        return E_POINTER;

    uri = calloc(1, sizeof(*uri));
    if (!uri)
        return E_OUTOFMEMORY;

    if (FAILED(hr = parse_uri_string(string, uri)))
    {
        free(uri);
        return hr;
    }

    uri->IUriRuntimeClass_iface.lpVtbl = &uri_vtbl;
    uri->ref = 1;

    *instance = &uri->IUriRuntimeClass_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE uri_factory_CreateWithRelativeUri(IUriRuntimeClassFactory *iface,
                                                                   HSTRING base_uri,
                                                                   HSTRING relative_uri,
                                                                   IUriRuntimeClass **instance)
{
    FIXME("iface %p, base_uri %s, relative_uri %s, instance %p stub!\n", iface,
          debugstr_hstring(base_uri), debugstr_hstring(relative_uri), instance);
    return E_NOTIMPL;
}

static const struct IUriRuntimeClassFactoryVtbl uri_factory_vtbl =
{
    uri_factory_QueryInterface,
    uri_factory_AddRef,
    uri_factory_Release,
    /* IInspectable methods */
    uri_factory_GetIids,
    uri_factory_GetRuntimeClassName,
    uri_factory_GetTrustLevel,
    /* IUriRuntimeClassFactory methods */
    uri_factory_CreateUri,
    uri_factory_CreateWithRelativeUri,
};

static struct iertutil iertutil =
{
    {&activation_factory_vtbl},
    {&uri_factory_vtbl},
    1
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    FIXME("clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out);
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    TRACE("classid %s, factory %p.\n", debugstr_hstring(classid), factory);
    *factory = &iertutil.IActivationFactory_iface;
    IUnknown_AddRef(*factory);
    return S_OK;
}
