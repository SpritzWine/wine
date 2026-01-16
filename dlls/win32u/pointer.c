/*
 * USER Pointer Message processing
 *
 * Copyright 2025 Hecheng Yu
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

#if 0
#pragma makedep unix
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"
#include <pthread.h>

WINE_DEFAULT_DEBUG_CHANNEL(pointer);

#define MAX_ACTIVE_POINTERS 10

struct pointer_info_entry
{
    BOOL active;
    UINT32 pointerId;
    POINTER_INPUT_TYPE pointerType;
    HWND hwndTarget;
    POINT ptPixelLocation;
    DWORD pointerFlags;
    DWORD dwTime;
    DWORD releaseTime;
    UINT message;
};

static struct pointer_info_entry pointer_cache[MAX_ACTIVE_POINTERS];
static pthread_mutex_t pointer_cache_mutex = PTHREAD_MUTEX_INITIALIZER;


/* Find a cache entry by pointer ID */
static struct pointer_info_entry *find_pointer_entry( UINT32 id )
{
    int i;
    for (i = 0; i < MAX_ACTIVE_POINTERS; i++)
    {
        if (pointer_cache[i].active && pointer_cache[i].pointerId == id)
            return &pointer_cache[i];
    }
    return NULL;
}

/* Find an empty cache slot */
static struct pointer_info_entry *get_free_pointer_entry( void )
{
    int i;
    for (i = 0; i < MAX_ACTIVE_POINTERS; i++)
    {
        if (!pointer_cache[i].active)
            return &pointer_cache[i];
    }
    WARN( "Pointer cache full!\n" );
    return NULL;
}

/* Store pointer information from hardware input */
void store_pointer_info( HWND hwnd, const INPUT *input, LPARAM lparam )
{
    struct pointer_info_entry *entry;
    UINT32 id;
    DWORD flags;
    DWORD msg;
    POINT pos;

    if (input->type != INPUT_HARDWARE) return;

    msg = input->hi.uMsg;

    /* Only handle WM_POINTER* messages */
    if (msg != WM_POINTERDOWN &&
        msg != WM_POINTERUPDATE &&
        msg != WM_POINTERUP)
        return;

    id = input->hi.wParamL;

    /* Reconstruct full 32-bit flags: add high bits based on message type */
    flags = input->hi.wParamH;
    if (msg == WM_POINTERDOWN)
        flags |= POINTER_FLAG_DOWN;
    else if (msg == WM_POINTERUPDATE)
        flags |= POINTER_FLAG_UPDATE;
    else if (msg == WM_POINTERUP)
        flags |= POINTER_FLAG_UP;

    /* lparam contains normalized position (0-65535) */
    pos.x = LOWORD(lparam);
    pos.y = HIWORD(lparam);

    TRACE("Storing pointer %u, msg=%u, flags=%u, pos=(%d,%d)\n",
          id, msg, flags, pos.x, pos.y);

    pthread_mutex_lock( &pointer_cache_mutex );

    entry = find_pointer_entry( id );

    if (msg == WM_POINTERDOWN)
    {
        /* New touch - allocate entry */
        if (!entry) entry = get_free_pointer_entry();
        if (entry)
        {
            entry->active = TRUE;
            entry->pointerId = id;
            entry->pointerType = PT_TOUCH; // Since currently only XI_TOUCH* triggers WM_POINTER*, it's okay to use hardcoded PT_TOUCH here.
            entry->hwndTarget = hwnd;
        }
    }

    if (entry)
    {
        entry->message = msg;
        entry->pointerFlags = flags;
        entry->ptPixelLocation.x = pos.x;
        entry->ptPixelLocation.y = pos.y;
        entry->dwTime = NtGetTickCount();

        if (msg == WM_POINTERUP)
        {
            entry->releaseTime = NtGetTickCount();
        }
    }

    pthread_mutex_unlock( &pointer_cache_mutex );
}

BOOL get_pointer_type( UINT32 id, POINTER_INPUT_TYPE *type )
{
    struct pointer_info_entry *entry;
    BOOL ret = FALSE;

    if (id == 1)
    {
        *type = PT_MOUSE;
        TRACE( "Pointer 1 is mouse\n" );
        return TRUE;
    }

    pthread_mutex_lock( &pointer_cache_mutex );

    entry = find_pointer_entry( id );
    if (entry && entry->active)
    {
        *type = entry->pointerType;
        TRACE( "Pointer %u type: %d\n", id, *type );
        ret = TRUE;
    }
    else
    {
        WARN( "Pointer %u not found\n", id );
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
    }

    pthread_mutex_unlock( &pointer_cache_mutex );
    return ret;
}

static void cleanup_expired_pointers( void )
{
    DWORD current_time = NtGetTickCount();

    for (int i = 0; i < MAX_ACTIVE_POINTERS; i++)
    {
        struct pointer_info_entry *entry = &pointer_cache[i];

        if (entry->active && entry->releaseTime > 0)
        {
            /* Clean up pointers released more than 500ms ago */
            if ((current_time - entry->releaseTime) > 500)
            {
                TRACE( "Pointer %u expired, cleaning up\n", entry->pointerId );
                entry->active = FALSE;
                entry->releaseTime = 0;
            }
        }
    }
}

BOOL get_pointer_touch_info( UINT32 id, POINTER_TOUCH_INFO *info )
{
    struct pointer_info_entry *entry;
    BOOL ret = FALSE;

    pthread_mutex_lock( &pointer_cache_mutex );
    cleanup_expired_pointers();
    entry = find_pointer_entry( id );
    if (entry && entry->active)
    {
        RECT virtual = NtUserGetVirtualScreenRect( MDT_RAW_DPI );
        int screen_width = virtual.right - virtual.left;
        int screen_height = virtual.bottom - virtual.top;

        memset( info, 0, sizeof(*info) );

        /* Fill POINTER_INFO */
        info->pointerInfo.pointerType = entry->pointerType;
        info->pointerInfo.pointerId = entry->pointerId;
        info->pointerInfo.frameId = 0;
        info->pointerInfo.pointerFlags = entry->pointerFlags;
        info->pointerInfo.sourceDevice = NULL;
        info->pointerInfo.hwndTarget = entry->hwndTarget;

        /* Convert normalized position (0-65535) to screen pixels */
        info->pointerInfo.ptPixelLocation.x = (entry->ptPixelLocation.x * screen_width) / 65536;
        info->pointerInfo.ptPixelLocation.y = (entry->ptPixelLocation.y * screen_height) / 65536;

        info->pointerInfo.ptHimetricLocation.x = 0;
        info->pointerInfo.ptHimetricLocation.y = 0;
        info->pointerInfo.ptPixelLocationRaw = info->pointerInfo.ptPixelLocation;
        info->pointerInfo.ptHimetricLocationRaw.x = 0;
        info->pointerInfo.ptHimetricLocationRaw.y = 0;
        info->pointerInfo.dwTime = entry->dwTime;
        info->pointerInfo.historyCount = 0;
        info->pointerInfo.InputData = 0;
        info->pointerInfo.dwKeyStates = 0;
        info->pointerInfo.PerformanceCount = 0;
        info->pointerInfo.ButtonChangeType = POINTER_CHANGE_NONE;

        /* Fill touch-specific info */
        info->touchFlags = TOUCH_FLAG_NONE;
        info->touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;

        /* Contact area - small rectangle around touch point */
        info->rcContact.left = info->pointerInfo.ptPixelLocation.x - 5;
        info->rcContact.top = info->pointerInfo.ptPixelLocation.y - 5;
        info->rcContact.right = info->pointerInfo.ptPixelLocation.x + 5;
        info->rcContact.bottom = info->pointerInfo.ptPixelLocation.y + 5;
        info->rcContactRaw = info->rcContact;

        info->orientation = 0;
        info->pressure = 1024;

        TRACE( "Retrieved pointer %u: type=%d, pos=(%d,%d), flags=%u\n",
               id, entry->pointerType,
               info->pointerInfo.ptPixelLocation.x,
               info->pointerInfo.ptPixelLocation.y,
               entry->pointerFlags );

        ret = TRUE;
    }
    else
    {
        WARN( "Pointer %u not found or inactive\n", id );
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
    }

    pthread_mutex_unlock( &pointer_cache_mutex );
    return ret;
}

BOOL get_pointer_frame_touch_info( UINT32 id, UINT32 *count, POINTER_TOUCH_INFO *info )
{
    struct pointer_info_entry *entry;
    UINT32 found_count = 0;
    UINT32 msg_flags;
    BOOL ret = FALSE;
    int i;

    pthread_mutex_lock( &pointer_cache_mutex );
    cleanup_expired_pointers();

    /* First, check if the requested pointer exists */
    entry = find_pointer_entry( id );
    if (!entry || !entry->active)
    {
        WARN( "Pointer %u not found or inactive\n", id );
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        pthread_mutex_unlock( &pointer_cache_mutex );
        return FALSE;
    }

    /* Count active touch pointers (DOWN, UPDATE, OR UP) - exclude already-processed UP events */
    for (i = 0; i < MAX_ACTIVE_POINTERS; i++)
    {
        if (pointer_cache[i].active &&
            pointer_cache[i].pointerType == PT_TOUCH &&
            (pointer_cache[i].pointerFlags & (POINTER_FLAG_DOWN | POINTER_FLAG_UPDATE | POINTER_FLAG_UP)))
        {
            found_count++;
        }
    }

    /* If info is NULL, caller just wants the count */
    if (!info)
    {
        *count = found_count;
        ret = TRUE;
    }
    else
    {
        /* Fill in touch info for all active pointers (DOWN, UPDATE, OR UP) */
        UINT32 idx = 0;
        for (i = 0; i < MAX_ACTIVE_POINTERS && idx < *count; i++)
        {
            if (pointer_cache[i].active &&
                pointer_cache[i].pointerType == PT_TOUCH &&
                (pointer_cache[i].pointerFlags & (POINTER_FLAG_DOWN | POINTER_FLAG_UPDATE | POINTER_FLAG_UP)))
            {
                /* Reuse get_pointer_touch_info logic */
                RECT virtual = NtUserGetVirtualScreenRect( MDT_RAW_DPI );
                int screen_width = virtual.right - virtual.left;
                int screen_height = virtual.bottom - virtual.top;
                struct pointer_info_entry *e = &pointer_cache[i];

                memset( &info[idx], 0, sizeof(info[idx]) );

                /* Fill POINTER_INFO */
                info[idx].pointerInfo.pointerType = e->pointerType;
                info[idx].pointerInfo.pointerId = e->pointerId;
                info[idx].pointerInfo.frameId = 0;

                /* Keep the state flags (DOWN/UPDATE/UP) */
                msg_flags = e->pointerFlags & (POINTER_FLAG_DOWN | POINTER_FLAG_UPDATE | POINTER_FLAG_UP);

                /* Add the message flags based on state */
                if (e->pointerFlags & POINTER_FLAG_DOWN)
                {
                    msg_flags |= POINTER_MESSAGE_FLAG_NEW | POINTER_MESSAGE_FLAG_INRANGE | POINTER_MESSAGE_FLAG_INCONTACT;
                    msg_flags |= POINTER_MESSAGE_FLAG_FIRSTBUTTON;
                }
                else if (e->pointerFlags & POINTER_FLAG_UPDATE)
                {
                    msg_flags |= POINTER_MESSAGE_FLAG_INRANGE | POINTER_MESSAGE_FLAG_INCONTACT;
                    msg_flags |= POINTER_MESSAGE_FLAG_FIRSTBUTTON;
                }
                else if (e->pointerFlags & POINTER_FLAG_UP)
                {
                    msg_flags |= POINTER_MESSAGE_FLAG_INRANGE;
                }

                /* First pointer in frame is PRIMARY - use POINTER_FLAG_PRIMARY not MESSAGE version */
                if (idx == 0)
                    msg_flags |= POINTER_FLAG_PRIMARY;

                msg_flags |= POINTER_FLAG_CONFIDENCE;

                info[idx].pointerInfo.pointerFlags = msg_flags;
                info[idx].pointerInfo.sourceDevice = NULL;
                info[idx].pointerInfo.hwndTarget = e->hwndTarget;

                /* Convert normalized position to screen pixels */
                info[idx].pointerInfo.ptPixelLocation.x = (e->ptPixelLocation.x * screen_width) / 65536;
                info[idx].pointerInfo.ptPixelLocation.y = (e->ptPixelLocation.y * screen_height) / 65536;

                info[idx].pointerInfo.ptHimetricLocation.x = 0;
                info[idx].pointerInfo.ptHimetricLocation.y = 0;
                info[idx].pointerInfo.ptPixelLocationRaw = info[idx].pointerInfo.ptPixelLocation;
                info[idx].pointerInfo.ptHimetricLocationRaw.x = 0;
                info[idx].pointerInfo.ptHimetricLocationRaw.y = 0;
                info[idx].pointerInfo.dwTime = e->dwTime;
                info[idx].pointerInfo.historyCount = 0;
                info[idx].pointerInfo.InputData = 0;
                info[idx].pointerInfo.dwKeyStates = 0;
                info[idx].pointerInfo.PerformanceCount = 0;
                info[idx].pointerInfo.ButtonChangeType = POINTER_CHANGE_NONE;

                /* Fill touch-specific info */
                info[idx].touchFlags = TOUCH_FLAG_NONE;
                info[idx].touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;

                info[idx].rcContact.left = info[idx].pointerInfo.ptPixelLocation.x - 5;
                info[idx].rcContact.top = info[idx].pointerInfo.ptPixelLocation.y - 5;
                info[idx].rcContact.right = info[idx].pointerInfo.ptPixelLocation.x + 5;
                info[idx].rcContact.bottom = info[idx].pointerInfo.ptPixelLocation.y + 5;
                info[idx].rcContactRaw = info[idx].rcContact;

                info[idx].orientation = 0;
                info[idx].pressure = (e->pointerFlags & POINTER_FLAG_UP) ? 0 : 1024;

                TRACE("[%u] id=%u flags=%u (DOWN=%d UPDATE=%d UP=%d) pos=(%d,%d) rcContact=(%dx%d) pressure=%u\n",
                      idx, e->pointerId, msg_flags,
                      !!(e->pointerFlags & POINTER_FLAG_DOWN),
                      !!(e->pointerFlags & POINTER_FLAG_UPDATE),
                      !!(e->pointerFlags & POINTER_FLAG_UP),
                      info[idx].pointerInfo.ptPixelLocation.x,
                      info[idx].pointerInfo.ptPixelLocation.y,
                      info[idx].rcContact.right - info[idx].rcContact.left,
                      info[idx].rcContact.bottom - info[idx].rcContact.top,
                      info[idx].pressure);

                idx++;
            }
        }

        *count = idx;
        ret = TRUE;
    }

    pthread_mutex_unlock( &pointer_cache_mutex );
    return ret;
}

/**********************************************************************
 * NtUserGetPointerType (win32u.@)
 */
BOOL WINAPI NtUserGetPointerType( UINT32 id, POINTER_INPUT_TYPE *type )
{
    TRACE( "id %u, type %p\n", id, type );
    if (!type)
    {
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    return get_pointer_type( id, type );
}

/**********************************************************************
 * NtUserGetPointerTouchInfo (win32u.@)
 */
BOOL WINAPI NtUserGetPointerTouchInfo( UINT32 id, POINTER_TOUCH_INFO *info )
{
    TRACE( "id %u, info %p\n", id, info );

    if (!info)
    {
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    return get_pointer_touch_info( id, info );
}

/**********************************************************************
 * NtUserGetPointerFrameTouchInfo (win32u.@)
 */
BOOL WINAPI NtUserGetPointerFrameTouchInfo( UINT32 id, UINT32 *count, POINTER_TOUCH_INFO *info )
{
    TRACE( "id %u, count %p, info %p\n", id, count, info );
    if (!count)
    {
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    return get_pointer_frame_touch_info( id, count, info );
}
