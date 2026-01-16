/*
 * MSDelta
 *
 * Copyright 2024 Vijay Kiran Kamuju
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

#include "windef.h"
#include "winbase.h"
#include "msdelta.h"
#include "wine/debug.h"


WINE_DEFAULT_DEBUG_CHANNEL(msdelta);


static WCHAR *strdupAW(const char *src)
{
    WCHAR *dst = NULL;
    if (src)
    {
        int len = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);
        if ((dst = malloc(len * sizeof(WCHAR))))
            MultiByteToWideChar(CP_ACP, 0, src, -1, dst, len);
    }
    return dst;
}

/*****************************************************
 *    ApplyDeltaA (MSDELTA.@)
 */
BOOL WINAPI ApplyDeltaA(DELTA_FLAG_TYPE flags, LPCSTR source_file,
                        LPCSTR delta_file, LPCSTR target_file)
{
    BOOL ret;
    WCHAR *source_fileW, *delta_fileW = NULL, *target_fileW = NULL;

    source_fileW = strdupAW(source_file);
    delta_fileW = strdupAW(delta_file);
    target_fileW = strdupAW(target_file);

    ret = ApplyDeltaW(flags, source_fileW, delta_fileW, target_fileW);

    free(source_fileW);
    free(delta_fileW);
    free(target_fileW);

    return ret;
}

BOOL WINAPI ApplyDeltaW(DELTA_FLAG_TYPE flags, LPCWSTR source_file,
                        LPCWSTR delta_file, LPCWSTR target_file)
{
    BOOL ret = FALSE;
    FIXME("(%llx,%s,%s,%s): stub!\n", flags, debugstr_w(source_file), debugstr_w(delta_file), debugstr_w(target_file));

    if (!source_file)
    {
        SetLastError(ERROR_INVALID_DATA);
        return ret;
    }
    SetLastError(ERROR_FILE_NOT_FOUND);

    return ret;
}
