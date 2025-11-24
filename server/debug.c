/*
 * Wine server debug support
 *
 * Copyright (C) 2025 Ralf Habacker
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

#include "debug.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#pragma GCC diagnostic ignored "-Wvla"

void _trace(const char *file, int line, const char *func, const char *fmt, ...)
{
    va_list args1;
    va_list args2;

    va_start( args1, fmt );
    va_copy( args2, args1 );
    char buf[1+vsnprintf( NULL, 0, fmt, args1 )];
    va_end( args1 );
    vsnprintf( buf, sizeof buf, fmt, args2 );
    va_end( args2 );

    fprintf( stderr, "trace:server:%s %s", func, buf );
}

void _trace_raw(const char *fmt, ...)
{
     va_list args1;
     va_list args2;

     va_start( args1, fmt );
     va_copy( args2, args1 );
     char buf[1+vsnprintf( NULL, 0, fmt, args1 )];
     va_end( args1 );
     vsnprintf( buf, sizeof buf, fmt, args2 );
     va_end( args2 );

     fprintf( stderr, "%s", buf );
}
