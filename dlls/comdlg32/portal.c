/*
 * Bridge comdlg32's classic file dialogs to the host's native file chooser
 * (the XDG desktop portal, org.freedesktop.portal.FileChooser).
 *
 * GetFileName95() calls COMDLG32_TryPortalFileDialog() before building Wine's
 * own dialog. When a portal is available we marshal the request, make a
 * __wine_unix_call into comdlg32.so (whose Rust client talks D-Bus), translate
 * the chosen host path back to a DOS path and fill the OPENFILENAME. When no
 * portal is on the bus, or the app uses customization the portal can't honor,
 * we return FALSE so Wine shows its built-in dialog unchanged.
 *
 * Copyright 2026 The SpritzWine project
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

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "commdlg.h"
#include "cderr.h"
#include "shlwapi.h"
#include "wine/debug.h"

#include "cdlg.h"
#include "filedlgbrowser.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(commdlg);

/* Wine path-mapping helpers, resolved from kernel32 at runtime so comdlg32
 * needs no extra import-lib entry. */
static char  * (CDECL *p_wine_get_unix_file_name)( LPCWSTR dos );
static WCHAR * (CDECL *p_wine_get_dos_file_name)( LPCSTR unix_path );

static BOOL load_path_funcs(void)
{
    static BOOL tried, ok;

    if (!tried)
    {
        HMODULE kernel32 = GetModuleHandleW( L"kernel32.dll" );
        if (kernel32)
        {
            p_wine_get_unix_file_name = (void *)GetProcAddress( kernel32, "wine_get_unix_file_name" );
            p_wine_get_dos_file_name  = (void *)GetProcAddress( kernel32, "wine_get_dos_file_name" );
        }
        ok = p_wine_get_unix_file_name && p_wine_get_dos_file_name;
        tried = TRUE;
    }
    return ok;
}

/* Opt out via environment, e.g. WINE_NO_PORTAL_FILEDIALOG=1. */
static BOOL portal_disabled(void)
{
    return GetEnvironmentVariableW( L"WINE_NO_PORTAL_FILEDIALOG", NULL, 0 ) != 0;
}

/* UTF-16 -> heap UTF-8 (free with free()); NULL/empty -> NULL. */
static char *utf8_dup( const WCHAR *str )
{
    char *out;
    int len;

    if (!str || !*str) return NULL;
    len = WideCharToMultiByte( CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL );
    if (!len) return NULL;
    if (!(out = malloc( len ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, str, -1, out, len, NULL, NULL );
    return out;
}

/* Serialize a Win32 filter list ("label\0pattern\0...\0\0") to the same
 * double-NUL-terminated form in UTF-8 for the Rust side (free with free()). */
static char *serialize_filters( const WCHAR *filter )
{
    const WCHAR *s;
    char *out;
    int nchars, len;

    if (!filter || !*filter) return NULL;

    s = filter;
    while (*s) s += lstrlenW( s ) + 1; /* walk label\0pattern\0... pairs */
    s++;                               /* include the terminating empty string */
    nchars = s - filter;               /* WCHARs incl. both trailing NULs */

    len = WideCharToMultiByte( CP_UTF8, 0, filter, nchars, NULL, 0, NULL, NULL );
    if (!len) return NULL;
    if (!(out = malloc( len ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, filter, nchars, out, len, NULL, NULL );
    return out;
}

/* Write the chosen DOS path into the OPENFILENAME, mirroring the native dialog's
 * single-selection result handling (lpstrFile, nFileOffset/nFileExtension,
 * lpstrFileTitle), honoring the A/W flavor. */
static void fill_ofn_result( FileOpenDlgInfos *info, const WCHAR *path )
{
    OPENFILENAMEW *ofnW = info->ofnInfos;

    if (ofnW->lpstrFile)
    {
        if (info->unicode)
            lstrcpynW( ofnW->lpstrFile, path, ofnW->nMaxFile );
        else
        {
            OPENFILENAMEA *ofnA = (OPENFILENAMEA *)info->ofnInfos;
            WideCharToMultiByte( CP_ACP, 0, path, -1, ofnA->lpstrFile, ofnA->nMaxFile, NULL, NULL );
        }
    }

    if (info->unicode)
    {
        const WCHAR *name = PathFindFileNameW( path );
        const WCHAR *ext  = PathFindExtensionW( path );
        ofnW->nFileOffset    = name - path;
        ofnW->nFileExtension = *ext ? (ext - path) + 1 : 0;
    }
    else
    {
        CHAR tmp[MAX_PATH];
        const CHAR *name, *ext;
        WideCharToMultiByte( CP_ACP, 0, path, -1, tmp, sizeof(tmp), NULL, NULL );
        name = PathFindFileNameA( tmp );
        ext  = PathFindExtensionA( tmp );
        ofnW->nFileOffset    = name - tmp;
        ofnW->nFileExtension = *ext ? (ext - tmp) + 1 : 0;
    }

    if (info->ofnInfos->lpstrFileTitle)
    {
        const WCHAR *title = PathFindFileNameW( path );
        if (info->unicode)
            lstrcpynW( ofnW->lpstrFileTitle, title, ofnW->nMaxFileTitle );
        else
        {
            OPENFILENAMEA *ofnA = (OPENFILENAMEA *)info->ofnInfos;
            WideCharToMultiByte( CP_ACP, 0, title, -1, ofnA->lpstrFileTitle, ofnA->nMaxFileTitle, NULL, NULL );
        }
    }
}

BOOL COMDLG32_TryPortalFileDialog( FileOpenDlgInfos *info, BOOL *result )
{
    struct file_chooser_params params = { 0 };
    char out_path[4096];
    char *initial_dir = NULL;
    WCHAR *dos = NULL;
    NTSTATUS status;
    DWORD flags = info->ofnInfos->Flags;
    BOOL handled = FALSE;

    /* Customization we can't express through the portal -> use Wine's dialog.
     * Multi-select is deferred to a later iteration. */
    if (flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE | OFN_ALLOWMULTISELECT))
        return FALSE;
    if (!COMDLG32_unixlib_initialized || portal_disabled() || !load_path_funcs())
        return FALSE;

    params.save  = (info->DlgInfos.dwDlgProp & FODPROP_SAVEDLG) != 0;
    params.title = utf8_dup( info->title );
    if (info->initdir && *info->initdir)
        params.initial_dir = initial_dir = p_wine_get_unix_file_name( info->initdir );
    if (params.save)
        params.current_name = utf8_dup( PathFindFileNameW( info->filename ) );
    params.filters  = serialize_filters( info->filter );
    params.out_path = out_path;
    params.out_size = sizeof(out_path);

    status = COMDLG32_CALL( file_chooser, &params );
    TRACE( "portal file chooser returned %#lx\n", status );

    free( (void *)params.title );
    free( (void *)params.current_name );
    free( (void *)params.filters );

    switch (status)
    {
    case STATUS_SUCCESS:
        dos = p_wine_get_dos_file_name( out_path );
        if (!dos)
        {
            /* Chosen file is outside every mapped drive; report as a cancel
             * rather than re-show Wine's dialog over the user's pick. */
            *result = FALSE;
            handled = TRUE;
            break;
        }
        if (info->ofnInfos->lpstrFile && (UINT)lstrlenW( dos ) >= info->ofnInfos->nMaxFile)
        {
            COMDLG32_SetCommDlgExtendedError( FNERR_BUFFERTOOSMALL );
            *result = FALSE;
        }
        else
        {
            fill_ofn_result( info, dos );
            *result = TRUE;
        }
        HeapFree( GetProcessHeap(), 0, dos );
        handled = TRUE;
        break;

    case STATUS_CANCELLED:
        *result = FALSE;
        handled = TRUE;
        break;

    default: /* STATUS_NOT_SUPPORTED and anything unexpected -> fall back */
        handled = FALSE;
        break;
    }

    if (initial_dir) HeapFree( GetProcessHeap(), 0, initial_dir );
    return handled;
}
