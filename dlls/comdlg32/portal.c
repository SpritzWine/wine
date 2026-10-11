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

static BOOL print_portal_disabled(void)
{
    return GetEnvironmentVariableW( L"WINE_NO_PORTAL_PRINTDIALOG", NULL, 0 ) != 0;
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

/* Shared core: marshal the request, make the unix call to the portal client,
 * and translate the chosen host path(s) back to DOS paths. Used by both the
 * classic and Vista-style dialogs. dos_out receives a double-NUL-terminated
 * list of DOS paths. */
enum portal_result COMDLG32_RunPortalChooser( BOOL save, BOOL multiple, BOOL directory,
        const WCHAR *title, const WCHAR *initial_dir, const WCHAR *current_name,
        const WCHAR *filter, ULONG current_filter,
        WCHAR *dos_out, DWORD dos_out_len, ULONG *out_filter )
{
    struct file_chooser_params params = { 0 };
    char out_paths[8192];
    char *initial_dir8 = NULL;
    const char *host;
    WCHAR *w = dos_out;
    DWORD remaining = dos_out_len;
    NTSTATUS status;
    enum portal_result ret;

    if (out_filter) *out_filter = 0;
    if (!COMDLG32_unixlib_initialized || portal_disabled() || !load_path_funcs())
        return PORTAL_FALLBACK;

    params.save      = save;
    params.multiple  = multiple;
    params.directory = directory;
    params.title     = utf8_dup( title );
    if (initial_dir && *initial_dir)
        params.initial_dir = initial_dir8 = p_wine_get_unix_file_name( initial_dir );
    if (save && current_name && *current_name)
        params.current_name = utf8_dup( current_name );
    params.filters        = serialize_filters( filter );
    params.current_filter = current_filter;
    params.out_paths      = out_paths;
    params.out_size       = sizeof(out_paths);

    status = COMDLG32_CALL( file_chooser, &params );
    TRACE( "portal file chooser returned %#lx\n", status );

    free( (void *)params.title );
    free( (void *)params.current_name );
    free( (void *)params.filters );
    if (initial_dir8) HeapFree( GetProcessHeap(), 0, initial_dir8 );

    switch (status)
    {
    case STATUS_SUCCESS:
        /* out_paths is a double-NUL list of UTF-8 host paths; translate each to
         * a DOS path and build a double-NUL WCHAR list in dos_out. */
        for (host = out_paths; *host; host += strlen(host) + 1)
        {
            WCHAR *dos = p_wine_get_dos_file_name( host );
            DWORD len;

            if (!dos) continue; /* outside every mapped drive: skip */
            len = lstrlenW( dos ) + 1;
            if (len < remaining)
            {
                memcpy( w, dos, len * sizeof(WCHAR) );
                w += len;
                remaining -= len;
            }
            HeapFree( GetProcessHeap(), 0, dos );
        }
        *w = 0; /* final terminator */

        if (w == dos_out)
        {
            /* Nothing usable (all outside mapped drives); report as a cancel
             * rather than re-show Wine's dialog over the user's pick. */
            ret = PORTAL_CANCELLED;
            break;
        }
        if (out_filter) *out_filter = params.out_filter;
        ret = PORTAL_CHOSEN;
        break;
    case STATUS_CANCELLED:
        ret = PORTAL_CANCELLED;
        break;
    default: /* STATUS_NOT_SUPPORTED or anything unexpected -> fall back */
        ret = PORTAL_FALLBACK;
        break;
    }
    return ret;
}

/* Print dialog: hand the pre-filled request to the portal client and report how
 * it ended. The DEVMODE/DEVNAMES work from the answer stays in printdlg.c. */
enum portal_result COMDLG32_RunPortalPrintDialog( const WCHAR *title, struct prepare_print_params *params )
{
    NTSTATUS status;

    if (!COMDLG32_unixlib_initialized || print_portal_disabled())
        return PORTAL_FALLBACK;

    params->title = utf8_dup( title );
    status = COMDLG32_CALL( prepare_print, params );
    TRACE( "portal print dialog returned %#lx\n", status );
    free( (void *)params->title );
    params->title = NULL;

    switch (status)
    {
    case STATUS_SUCCESS:   return PORTAL_CHOSEN;
    case STATUS_CANCELLED: return PORTAL_CANCELLED;
    default:               return PORTAL_FALLBACK;
    }
}

/* Folder-picker entry exported for shell32's SHBrowseForFolder, which lives in
 * another DLL and so can't reach COMDLG32_RunPortalChooser directly. Picks a single
 * directory through the host portal. Returns 1 when the user chose a folder (dos_out
 * receives its DOS path), 0 when they cancelled, and -1 to tell the caller to fall
 * back to its own dialog (no portal on the bus, or a request it can't honor).
 * COMDLG32_RunPortalChooser already gates on portal availability. */
INT WINAPI COMDLG32_PortalPickFolder( const WCHAR *title, const WCHAR *initial_dir,
                                      WCHAR *dos_out, DWORD dos_out_len )
{
    switch (COMDLG32_RunPortalChooser( FALSE, FALSE, TRUE, title, initial_dir,
                                       NULL, NULL, 0, dos_out, dos_out_len, NULL ))
    {
    case PORTAL_CHOSEN:    return 1;
    case PORTAL_CANCELLED: return 0;
    default:               return -1;
    }
}

/* Fill an OFN_ALLOWMULTISELECT result from a double-NUL list of full DOS paths.
 * Win32 returns one full path for a single file, or "dir\0name1\0name2\0\0" for
 * several (the directory is taken from the first entry). Honors the A/W flavor.
 * Returns FALSE (and sets FNERR_BUFFERTOOSMALL) when it doesn't fit. */
static BOOL fill_ofn_result_multi( FileOpenDlgInfos *info, const WCHAR *list )
{
    OPENFILENAMEW *ofnW = info->ofnInfos;
    WCHAR dir[MAX_PATH], *blob, *p;
    const WCHAR *path;
    SIZE_T wlen = 1; /* final terminator */
    UINT count = 0, dirlen;
    BOOL ok = TRUE;

    for (path = list; *path; path += lstrlenW(path) + 1) count++;
    if (count <= 1)
    {
        /* Single selection: plain full path, as the single-select API. */
        if (ofnW->lpstrFile && (UINT)lstrlenW(list) >= ofnW->nMaxFile)
        {
            COMDLG32_SetCommDlgExtendedError(FNERR_BUFFERTOOSMALL);
            return FALSE;
        }
        fill_ofn_result(info, list);
        return TRUE;
    }

    lstrcpynW(dir, list, MAX_PATH);
    PathRemoveFileSpecW(dir);
    dirlen = lstrlenW(dir);

    wlen += dirlen + 1;
    for (path = list; *path; path += lstrlenW(path) + 1)
        wlen += lstrlenW(PathFindFileNameW(path)) + 1;

    if (!(blob = malloc(wlen * sizeof(WCHAR)))) return FALSE;
    p = blob;
    lstrcpyW(p, dir);
    p += dirlen + 1;
    for (path = list; *path; path += lstrlenW(path) + 1)
    {
        lstrcpyW(p, PathFindFileNameW(path));
        p += lstrlenW(p) + 1;
    }
    *p = 0;

    if (ofnW->lpstrFile)
    {
        if (info->unicode)
        {
            if (wlen > ofnW->nMaxFile) ok = FALSE;
            else
            {
                memcpy(ofnW->lpstrFile, blob, wlen * sizeof(WCHAR));
                ofnW->nFileOffset = dirlen + 1;
            }
        }
        else
        {
            OPENFILENAMEA *ofnA = (OPENFILENAMEA *)info->ofnInfos;
            int alen = WideCharToMultiByte(CP_ACP, 0, blob, wlen, NULL, 0, NULL, NULL);
            if ((UINT)alen > ofnA->nMaxFile) ok = FALSE;
            else
            {
                WideCharToMultiByte(CP_ACP, 0, blob, wlen, ofnA->lpstrFile, ofnA->nMaxFile, NULL, NULL);
                /* byte offset of the first name = length of the A directory + NUL */
                ofnW->nFileOffset = WideCharToMultiByte(CP_ACP, 0, dir, -1, NULL, 0, NULL, NULL);
            }
        }
    }
    ofnW->nFileExtension = 0;
    free(blob);

    if (!ok) COMDLG32_SetCommDlgExtendedError(FNERR_BUFFERTOOSMALL);
    return ok;
}

void COMDLG32_AppendDefaultExt( WCHAR *path, const WCHAR *defext )
{
    if (!defext || !*defext) return;
    if (*PathFindExtensionW( path )) return; /* already has an extension */
    lstrcatW( path, L"." );
    lstrcatW( path, defext );
    path[ lstrlenW( path ) + 1 ] = 0;        /* re-terminate the single-entry list */
}

BOOL COMDLG32_TryPortalFileDialog( FileOpenDlgInfos *info, BOOL *result )
{
    DWORD flags = info->ofnInfos->Flags;
    BOOL multiple = (flags & OFN_ALLOWMULTISELECT) != 0;
    BOOL save = (info->DlgInfos.dwDlgProp & FODPROP_SAVEDLG) != 0;
    const WCHAR *current_name;
    WCHAR dos[8192];
    ULONG out_filter = 0;

    /* Customization we can't express through the portal -> use Wine's dialog. */
    if (flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE))
        return FALSE;

    current_name = (save && info->filename) ? PathFindFileNameW( info->filename ) : NULL;

    switch (COMDLG32_RunPortalChooser( save, multiple, FALSE,
                                       info->title, info->initdir, current_name,
                                       info->filter, info->ofnInfos->nFilterIndex,
                                       dos, ARRAY_SIZE(dos), &out_filter ))
    {
    case PORTAL_CHOSEN:
        if (out_filter) info->ofnInfos->nFilterIndex = out_filter;
        if (multiple)
            *result = fill_ofn_result_multi( info, dos );
        else
        {
            if (save) COMDLG32_AppendDefaultExt( dos, info->defext );
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
        }
        return TRUE;

    case PORTAL_CANCELLED:
        *result = FALSE;
        return TRUE;

    default: /* PORTAL_FALLBACK */
        return FALSE;
    }
}
