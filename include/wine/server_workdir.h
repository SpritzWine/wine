/*
 * Wine server directory handling
 *
 * Copyright 2022 Konstantin Demin
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

#ifndef __WINE_WINE_SERVER_WORKDIR_H
#define __WINE_WINE_SERVER_WORKDIR_H 1

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>


static int wineserver_workdir_test_path( struct stat *st, int per_user )
{
    if (!st) return 1;

    /* NB: return codes must be distinct and aligned with wineserver_workdir_try_path() */

    if (!S_ISDIR( st->st_mode ))
    {
        errno = ENOTDIR;
        return 3;
    }

    if (!per_user) return 0;

    if (st->st_uid != getuid()) return 4;
    if (st->st_mode & 077) return 5;
    if ((st->st_mode & 0700) != 0700) return 6;

    return 0;
}

static int wineserver_workdir_try_path( const char *path, struct stat *st, int per_user )
{
    if ((!path) || (!st)) return 1;

    (void) memset( st, 0, sizeof(struct stat) );
    errno = 0;
    if (lstat( path, st ) == -1) return 2;

    return wineserver_workdir_test_path( st, per_user );
}

/*
 * try following locations for wineserver directory:
 * - ${XDG_RUNTIME_DIR}/wine
 * - /run/user/${uid}/wine
 * - ${TMPDIR}/wine - only if ${TMPDIR} is per-user
 * - ${TMPDIR}/.wine-${uid}
 * - /tmp/.wine-${uid} - default behavior
 */

static inline char *wineserver_workdir_impl(void)
{
    const char *rootdir = NULL;
    char *workdir = NULL;
    struct stat st1;

    /* try "${XDG_RUNTIME_DIR}/wine" */
    do {
        rootdir = getenv( "XDG_RUNTIME_DIR" );
        if (!rootdir) break;
        if ((!rootdir[0]) || (rootdir[0] != '/')) break;

        if (wineserver_workdir_try_path( rootdir, &st1, 1 /* per-user */ )) break;

        errno = 0;
        if (asprintf( &workdir, "%s/wine", rootdir ) < 0)
        {
            if (errno == 0) errno = ENOMEM;
            return NULL;
        }

        errno = 0;
        return workdir;
    } while (0);

    /* try "/run/user/${uid}/wine" */
    do {
        const size_t uid_len = 11; /* maximum uid length */
        const size_t full_len = sizeof( "/run/user/" ) + uid_len + sizeof( "/wine" ) + 1 /* NUL */;

        errno = 0;
        workdir = (char *) malloc( full_len );
        if (!workdir)
        {
            if (errno == 0) errno = ENOMEM;
            return NULL;
        }
        (void) memset( workdir, 0, full_len );

        /* NB: safe enough - space is already allocated */
        (void) sprintf( workdir, "/run/user/%u", getuid() );
        if (wineserver_workdir_try_path( workdir, &st1, 1 /* per-user */ ))
        {
            free( workdir ); workdir = NULL;
            break;
        }

        /* NB: safe enough - space is already allocated */
        (void) strcat( workdir, "/wine" );

        errno = 0;
        return workdir;
    } while (0);

    /* try somewhere in ${TMPDIR}/ */
    do {
        rootdir = getenv( "TMPDIR" );
        if (!rootdir) break;
        if ((!rootdir[0]) || (rootdir[0] != '/')) break;

        /* verify directory existence */
        if (wineserver_workdir_try_path( rootdir, &st1, 0 /* shared */ )) break;

        if (wineserver_workdir_test_path( &st1, 1 /* per-user */ ))
        {
            /* ${TMPDIR} is NOT per-user - use "${TMPDIR}/.wine-${uid}" */
            errno = 0;
            if (asprintf( &workdir, "%s/.wine-%u", rootdir, getuid() ) < 0)
            {
                if (errno == 0) errno = ENOMEM;
                return NULL;
            }
        }
        else
        {
            /* ${TMPDIR} is per-user - use "${TMPDIR}/wine" */
            errno = 0;
            if (asprintf( &workdir, "%s/wine", rootdir ) < 0)
            {
                if (errno == 0) errno = ENOMEM;
                return NULL;
            }
        }

        errno = 0;
        return workdir;
    } while (0);

    /* LAST RESORT: use "/tmp/.wine-${uid}" */
    errno = 0;
    if (asprintf( &workdir, "/tmp/.wine-%u", getuid() ) < 0)
    {
        if (errno == 0) errno = ENOMEM;
        return NULL;
    }

    errno = 0;
    return workdir;
}

static char *__wineserver_workdir = NULL;
static __attribute__((noinline)) void wineserver_workdir_once(void)
{
    __wineserver_workdir = wineserver_workdir_impl();
}

static __attribute__((noinline)) char *wineserver_workdir(void)
{
    static pthread_once_t _once = PTHREAD_ONCE_INIT;

    (void) pthread_once( &_once, wineserver_workdir_once );
    return __wineserver_workdir;
}

#endif /* __WINE_WINE_SERVER_WORKDIR_H */
