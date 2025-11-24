/*
 * Wine server debug
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

#ifndef __WINE_SERVER_DEBUG_H
#define __WINE_SERVER_DEBUG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WINE_NO_TRACE_MSGS
void _trace(const char *file, int line, const char *func, const char *fmt, ...);
void _trace_raw(const char *fmt, ...);
#define TRACE(...) _trace(__FILE__, __LINE__, __func__, __VA_ARGS__)
#define TRACE_RAW(...) _trace_raw(__VA_ARGS__)
#else
#define TRACE(...) do { } while(0)
#define TRACE_RAW(...) do { } while(0)
#endif

#ifdef __cplusplus
}
#endif

#endif
