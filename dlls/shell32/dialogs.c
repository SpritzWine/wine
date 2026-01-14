/*
 *	common shell dialogs
 *
 * Copyright 2000 Juergen Schmied
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

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <assert.h>
#include "winerror.h"
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "wingdi.h"
#include "winuser.h"
#include "commdlg.h"
#include "wine/debug.h"

#include "shellapi.h"
#include "shlobj.h"
#include "shell32_main.h"
#include "shresdef.h"

/* RunFileDlg flags */
#define RFF_NOBROWSE        0x01
#define RFF_NODEFAULT       0x02
#define RFF_CALCDIRECTORY   0x04
#define RFF_NOLABEL         0x08
#define RFF_NOSEPARATEMEM   0x20  /* NT only */

/* RunFileFlg notification structure */
typedef struct
{
    NMHDR hdr;
    const char *lpFile;
    const char *lpDirectory;
    int nShow;
} NM_RUNFILEDLG;

/* RunFileDlg notification return values */
#define RF_OK      0x00
#define RF_CANCEL  0x01
#define RF_RETRY   0x02

typedef struct
    {
	HWND hwndOwner ;
	HICON hIcon ;
	LPCWSTR lpstrDirectory ;
	LPCWSTR lpstrTitle ;
	LPCWSTR lpstrDescription ;
	UINT uFlags ;
    } RUNFILEDLGPARAMS ;

typedef BOOL (WINAPI * LPFNOFN) (OPENFILENAMEW *) ;

WINE_DEFAULT_DEBUG_CHANNEL(shell);
static INT_PTR CALLBACK RunDlgProc (HWND, UINT, WPARAM, LPARAM) ;
static void FillList (HWND, char *, BOOL) ;

#define IMAGE_LIST_INDEX_INVALID_VALUE -1

enum enumActionResult { eDoneSuccessfullyWithAcceptableConfirmation, eDoneSuccessfully,
    eNotDoneNoError, eNotDoneErrorNotSerious, eNotDoneSeriousError };

static const WCHAR extended_length_path_prefix[] = { '\\', '\\', '?', '\\', 0 };
static const WCHAR system_root_var[] = { '%', 'S', 'y', 's', 't', 'e', 'm', 'R', 'o',
    'o', 't', '%', 0 };
static const WCHAR shell32_dll_file_name[] = { 'S', 'H', 'E', 'L', 'L', '3', '2',
    '.', 'd', 'l', 'l', 0 };
static const WCHAR file_extension_ico[] = { '.', 'i', 'c', 'o', 0 };
static const WCHAR backslash_text[] = { '\\', 0 };
static const WCHAR mask_integer[] = { '%', 'i', 0 };
static const WCHAR trim_chars[] = { '"', '\t', ' ', 0 };
static const WCHAR mask_s[] = { '%', 's', 0 };

struct tagIconNamesStruct
{
    union
    {
        INT index;
        WCHAR *name;
    } iconReferenceUnion;
    BOOL icon_reference_to_index;
    INT image_list_index;
    struct tagIconNamesStruct *next;
};


struct tagIconDialogStruct
{
    HWND parent_of_pick_icon_dialog;
    WCHAR *path_filename_allocated;
    UINT path_filename_max_chars;
    UINT new_path_filename_max_length;
    BOOL path_filename_changed_without_data_update;
    BOOL bad_input_error_message_showed;
    INT icon_index;
    DWORD error_code;
    BOOL ignore_notifications;
    struct tagIconNamesStruct *first, *last;
};


static BOOL PickIconDlg_ErrorCodeMeansSeriousError(const DWORD error_code)
{
    switch(error_code)
    {
        case ERROR_OUTOFMEMORY:
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_INTERNAL_ERROR:
        case ERROR_MOD_NOT_FOUND:
        case ERROR_PROC_NOT_FOUND:
        case ERROR_DLL_INIT_FAILED:
            return TRUE;
        default:
            return FALSE;
    }
}


static BOOL PickIconDlg_IsIcoFile(const WCHAR *path_filename)
{
    int text_length;
    text_length = lstrlenW(path_filename);
    if(text_length >= 4)
    {
        if(lstrcmpiW(&path_filename[text_length - 4], file_extension_ico) == 0)
            return TRUE;
    }
    return FALSE;
}


static enum enumActionResult PickIconDlg_ClearIconsData(const HWND dialog)
{
    HWND control;
    HIMAGELIST image_list;
    BOOL deleted;
    struct tagIconNamesStruct *current_record;
    struct tagIconDialogStruct *dialog_data =
        (struct tagIconDialogStruct *) GetWindowLongPtrW(dialog, DWLP_USER);
    current_record = dialog_data->first;
    while (current_record != NULL)
    {
        struct tagIconNamesStruct *record_to_delete;
        if(!current_record->icon_reference_to_index)
            free(current_record->iconReferenceUnion.name);
        record_to_delete = current_record;
        current_record = current_record->next;
        free(record_to_delete);
    }
    dialog_data->first = dialog_data->last = NULL;
    control = GetDlgItem(dialog, IDC_PICKICONDLG_ICONS);
    deleted = SendMessageW(control, LVM_DELETEALLITEMS,(WPARAM) 0, (LPARAM) 0);
    if(!deleted)
        dialog_data->error_code = ERROR_INTERNAL_ERROR;
    image_list = (HIMAGELIST) SendMessageW(control, LVM_SETIMAGELIST, (WPARAM)(0),
        (LPARAM)(HIMAGELIST)(((void *) 0)));
    if(image_list != NULL)
    {
        BOOL destroyed = ImageList_Destroy(image_list);
        assert(destroyed);
    }
    return (dialog_data->error_code == ERROR_SUCCESS) ? eDoneSuccessfully :
        eNotDoneSeriousError;
}


static WCHAR *PickIconDlg_CreateExpanded(const WCHAR *just_path_with_backslash_at_end,
    const WCHAR *file_name)
{
    WCHAR *return_value, *unexpanded;
    int length_path, length_file_name;
    DWORD required_expanded_size, number;
    length_path = lstrlenW(just_path_with_backslash_at_end);
    length_file_name = lstrlenW(file_name);
    unexpanded = malloc((length_path + length_file_name + 1) * sizeof(WCHAR));
    if(unexpanded == NULL)
        return NULL;
    lstrcpyW(unexpanded, just_path_with_backslash_at_end);
    lstrcatW(unexpanded, file_name);
    required_expanded_size = ExpandEnvironmentStringsW(unexpanded, NULL, 0);
    return_value = malloc(required_expanded_size * sizeof(WCHAR));
    if(return_value == NULL)
    {
        free(unexpanded);
        return NULL;
    }
    number = ExpandEnvironmentStringsW(unexpanded, return_value,
        required_expanded_size * sizeof(WCHAR));
    if(number == 0)
    {
        if(return_value != NULL)
            free(return_value);
        return_value = NULL;
    }
    free(unexpanded);
    return return_value;
}


static WCHAR *PickIconDlg_GetSystemDirectoryPathFilename(const WCHAR *file_name,
    const BOOL with_extended_length_path_prefix, const BOOL use_environment_variable)
{
    DWORD chars_needed;
    UINT size = GetSystemDirectoryW(NULL, 0);
    UINT file_name_chars = lstrlenW(file_name);
    WCHAR *buffer;
    chars_needed = size + file_name_chars + 1;
    if(with_extended_length_path_prefix)
        chars_needed += 4;
    buffer = malloc(chars_needed * sizeof(WCHAR));
    if(buffer != NULL)
    {
        if(!with_extended_length_path_prefix)
            GetSystemDirectoryW(buffer, size);
        else
        {
            lstrcpyW(buffer, extended_length_path_prefix);
            GetSystemDirectoryW(&buffer[4], size);
        }
        lstrcatW(buffer, backslash_text);
        lstrcatW(buffer, file_name);
        if(use_environment_variable)
        {
            WCHAR *system_root_var_expanded;
            DWORD result;
            WCHAR *match;
            DWORD index, system_root_var_expanded_chars;
            DWORD buffer_chars, copy_bytes;
            const DWORD system_root_var_size = ARRAYSIZE(system_root_var) - 1;
            result = ExpandEnvironmentStringsW(system_root_var, NULL, 0);
            system_root_var_expanded = malloc(result * sizeof(WCHAR));
            if(system_root_var_expanded == NULL)
            {
                free(buffer);
                return NULL;
            }
            result = ExpandEnvironmentStringsW(system_root_var, system_root_var_expanded,
                result * sizeof(WCHAR));
            assert(result > 0);
            buffer_chars = lstrlenW(buffer);
            CharUpperW(buffer);
            CharUpperW(system_root_var_expanded);
            match = wcsstr(buffer, system_root_var_expanded);
            if(match == NULL)
            {
                free(system_root_var_expanded);
                return buffer;
            }
            index = match - buffer;
            system_root_var_expanded_chars = lstrlenW(system_root_var_expanded);
            chars_needed = buffer_chars + system_root_var_size -
                system_root_var_expanded_chars + 1;
            if(chars_needed > buffer_chars)
            {
                WCHAR *buffer2 = (WCHAR *) realloc(buffer, chars_needed * sizeof(WCHAR));
                if(buffer2 == NULL)
                {
                    if(buffer != NULL) free(buffer);
                    free(system_root_var_expanded);
                    return NULL;
                }
                buffer = buffer2;
            }
            copy_bytes = sizeof(WCHAR) * (buffer_chars - index -
                system_root_var_expanded_chars + 1);
            memcpy(&buffer[index + system_root_var_size],
                &buffer[index + system_root_var_expanded_chars], copy_bytes);
            copy_bytes = system_root_var_size * sizeof(WCHAR);
            memcpy(&buffer[index], system_root_var, copy_bytes);
        }
    }
    return buffer;
}


static WCHAR *PickIconDlg_CreateExpandedOrCompletedPathFileName(const WCHAR *path_elementname,
    const BOOL with_extended_length_path_prefix)
{
    WCHAR *return_value;
    if(PathIsFileSpecW(path_elementname))
    {
        WCHAR path1[25];
        static const WCHAR back_slash[] = { '\\', 0 };
        path1[0] = 0;
        if(with_extended_length_path_prefix)
            lstrcatW(path1, extended_length_path_prefix);
        lstrcatW(path1, system_root_var);
        lstrcatW(path1, back_slash);
        return_value = PickIconDlg_CreateExpanded(path1, path_elementname);
        if(return_value)
            if(GetFileAttributesW(return_value) == INVALID_FILE_ATTRIBUTES)
            {
                free(return_value);
                return_value =
                    PickIconDlg_GetSystemDirectoryPathFilename(path_elementname, with_extended_length_path_prefix,
                        FALSE);
            }
    }
    else
    {
        DWORD result;
        result = ExpandEnvironmentStringsW(path_elementname, NULL, 0);
        return_value = malloc((result + 4) * sizeof(WCHAR));
        if(return_value)
        {
            WCHAR *resolved = return_value;
            int index;
            if(with_extended_length_path_prefix)
                lstrcpyW(resolved, extended_length_path_prefix);
            index = with_extended_length_path_prefix ? 4 : 0;
            result = ExpandEnvironmentStringsW(path_elementname, &resolved[index],
                result * sizeof(WCHAR));
            assert(result > 0);
        }
    }
    return return_value;
}


static enum enumActionResult PickIconDlg_UpdateImagesDataUseIcoFile(const HWND dialog,
    const WCHAR *extended_length_path_filename)
{
    ICONINFO icon_info;
    HICON icon;
    HWND listview;
    HIMAGELIST image_list = NULL, old_image_list;
    HINSTANCE instance;
    enum enumActionResult result;
    BOOL call_was_successful;
    struct tagIconDialogStruct *dialog_data = (VOID *) GetWindowLongPtrW(dialog, DWLP_USER);
    assert(PickIconDlg_IsIcoFile(extended_length_path_filename));
    instance = (HINSTANCE) GetWindowLongPtrW(dialog, GWLP_HINSTANCE);
    icon = LoadImageW(instance, extended_length_path_filename, IMAGE_ICON, 0, 0,
        LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if(icon == NULL)
    {
        dialog_data->error_code = GetLastError();
        if(dialog_data->error_code == ERROR_SUCCESS)
            return eDoneSuccessfully;
        return PickIconDlg_ErrorCodeMeansSeriousError(dialog_data->error_code) ?
            eNotDoneSeriousError : eNotDoneErrorNotSerious;
    }
    listview = GetDlgItem(dialog, IDC_PICKICONDLG_ICONS);
    call_was_successful = GetIconInfo(icon, &icon_info);
    if(call_was_successful)
    {
        INT index;
        INT icon_size_y, icon_size_x;
        BOOL successful;
        icon_size_y = GetSystemMetrics(SM_CYICON);
        icon_size_x = GetSystemMetrics(SM_CXICON);
        SendMessageW(listview, LVM_SETICONSPACING, 0,
            MAKELONG(icon_size_x * 14 / 10, icon_size_y * 17 / 10));
        image_list = ImageList_Create(icon_size_x, icon_size_y,
            ILC_MASK | ILC_COLOR24, 1, 1);
        if(image_list != NULL)
        {
            index = ImageList_Add(image_list, icon_info.hbmColor, icon_info.hbmMask);
            if(index == -1)
                dialog_data->error_code = ERROR_INTERNAL_ERROR;
        }
        else
            dialog_data->error_code = GetLastError();
        successful = DeleteObject(icon_info.hbmColor);
        if(!successful)
        {
            if(dialog_data->error_code == ERROR_SUCCESS)
                dialog_data->error_code = GetLastError();
        }
        successful = DeleteObject(icon_info.hbmMask);
        if(!successful)
        {
            if(dialog_data->error_code == ERROR_SUCCESS)
                dialog_data->error_code = GetLastError();
        }
    }
    else
        dialog_data->error_code = GetLastError();
    if(!DestroyIcon(icon))
    {
        if(dialog_data->error_code == ERROR_SUCCESS)
            dialog_data->error_code = GetLastError();
    }
    if(dialog_data->error_code == ERROR_SUCCESS)
    {
        INT index;
        LVITEMW lv = { 0 };
        static const WCHAR text_zero[] = { '0', 0 };
        result = PickIconDlg_ClearIconsData(dialog);
        if(result != eDoneSuccessfully)
        {
            BOOL destroyed = ImageList_Destroy(image_list);
            assert(destroyed);
            return result;
        }
        old_image_list = (HIMAGELIST) SendMessageW(listview, LVM_SETIMAGELIST,
            LVSIL_NORMAL, (LPARAM) (HIMAGELIST) image_list);
        if(old_image_list != NULL)
        {
            BOOL successful = ImageList_Destroy(old_image_list);
            assert(successful);
        }
        lv.mask = LVIF_IMAGE | LVIF_TEXT | LVIF_PARAM;
        lv.iImage = 0;
        lv.lParam = 0;
        lv.pszText = (WCHAR *) text_zero;
        index = SendMessageW(listview, LVM_INSERTITEMW, 0, (LPARAM) &lv);
        if(index == -1)
            dialog_data->error_code = ERROR_INTERNAL_ERROR;
    }
    else if(call_was_successful)
    {
        BOOL successful = ImageList_Destroy(image_list);
        assert(successful);
    }
    if(dialog_data->error_code == ERROR_SUCCESS)
    {
        size_t struct_size;
        struct_size = sizeof(struct tagIconNamesStruct);
        dialog_data->first = dialog_data->last = malloc(struct_size);
        if(dialog_data->first != NULL)
        {
            LVITEMW lv_item;
            dialog_data->first->icon_reference_to_index = TRUE;
            dialog_data->first->iconReferenceUnion.index = 0;
            dialog_data->first->image_list_index = 0;
            dialog_data->first->next = NULL;
            lv_item.stateMask = LVIS_FOCUSED | LVIS_SELECTED;
            lv_item.state = LVIS_FOCUSED | LVIS_SELECTED;
            SendMessageW(listview, LVM_SETITEMSTATE, 0, (LPARAM) &lv_item);
        }
        else
            dialog_data->error_code = ERROR_OUTOFMEMORY;
    }
    if(dialog_data->error_code == ERROR_SUCCESS)
        return eDoneSuccessfully;
    else
        return PickIconDlg_ErrorCodeMeansSeriousError(dialog_data->error_code) ?
            eNotDoneSeriousError : eNotDoneErrorNotSerious;
}


/* EnumResourceNames with LoadImageNames as parameter must be called before */
/* returns FALSE if error occured */
static BOOL PickIconDlg_AddIconsToListViewUseLibrary(const HMODULE module, const HWND dialog)
{
    INT record_counter;
    INT icon_size_y, icon_size_x;
    ICONINFO icon_info;
    HWND listview;
    HIMAGELIST image_list, old_image_list;
    LVITEMW lv = { 0 };
    BOOL successful;
    struct tagIconDialogStruct *dialog_data = (VOID *) GetWindowLongPtrW(dialog, DWLP_USER);
    struct tagIconNamesStruct *current_icon_name = dialog_data->first;
    icon_size_y = GetSystemMetrics(SM_CYICON);
    icon_size_x = GetSystemMetrics(SM_CXICON);
    image_list = ImageList_Create(icon_size_x, icon_size_y, ILC_MASK | ILC_COLOR24, 1, 100);
    if(image_list == NULL)
    {
        dialog_data->error_code = ERROR_INTERNAL_ERROR;
        return FALSE;
    }
    listview = GetDlgItem(dialog, IDC_PICKICONDLG_ICONS);
    while((current_icon_name != NULL) && (dialog_data->error_code == ERROR_SUCCESS))
    {
        HICON icon;
        if(current_icon_name->icon_reference_to_index)
            icon = LoadImageW(module, (LPCWSTR) (INT_PTR) current_icon_name->iconReferenceUnion.index,
                IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        else
            icon = LoadImageW(module, current_icon_name->iconReferenceUnion.name,
                IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        if(icon == NULL)
        {
            dialog_data->error_code = GetLastError();
            if(dialog_data->error_code == ERROR_MOD_NOT_FOUND)
                dialog_data->error_code = ERROR_SUCCESS;
        }
        else
        {
            successful = GetIconInfo(icon, &icon_info);
            if(successful && (icon_info.hbmColor == NULL))
            {
                /* The size must be a multiple of 16 bits (2 bytes). */
                int number_of_WORD_needed = (icon_size_x + 15) / 16;
                int stride = number_of_WORD_needed * 2;
                int buffer_size = stride * icon_size_y;
                void *bits = calloc(buffer_size, 1);
                successful = bits != NULL;
                if(!successful)
                {
                    DeleteObject(icon_info.hbmMask);
                    dialog_data->error_code = ERROR_OUTOFMEMORY;
                }
                else
                {
                    icon_info.hbmColor = CreateBitmap(icon_size_x,
                        icon_size_y, 1, 1, bits);
                    free(bits);
                }
            }
            if(successful)
            {
                current_icon_name->image_list_index =
                    ImageList_Add(image_list, icon_info.hbmColor,
                    icon_info.hbmMask);
                if(current_icon_name->image_list_index == -1)
                    dialog_data->error_code = ERROR_INTERNAL_ERROR;
                successful = DeleteObject(icon_info.hbmColor);
                if(!successful)
                {
                    if(dialog_data->error_code == ERROR_SUCCESS)
                        dialog_data->error_code = GetLastError();
                }
                successful = DeleteObject(icon_info.hbmMask);
                if(!successful)
                {
                    if(dialog_data->error_code == ERROR_SUCCESS)
                        dialog_data->error_code = GetLastError();
                }
            }
            successful = DestroyIcon(icon);
            if(!successful)
            {
                if(dialog_data->error_code == ERROR_SUCCESS)
                    dialog_data->error_code = GetLastError();
            }
        }
        current_icon_name = current_icon_name->next;
    }
    if(dialog_data->error_code != ERROR_SUCCESS)
    {
        ImageList_Destroy(image_list);
        return FALSE;
    }
#ifndef LVS_EX_HIDELABELS_IMPLEMENTED
    SendMessageW(listview, LVM_SETICONSPACING, 0,
        (LPARAM) MAKELONG(icon_size_x * 14 / 10, icon_size_y * 17 / 10));
#else
    SendMessageW(listview, LVM_SETICONSPACING, 0,
            (LPARAM) MAKELONG(icon_size_x * 12 / 10, icon_size_y * 12 / 10));
#endif
    old_image_list = (HIMAGELIST) SendMessageW(listview, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM) image_list);
    if(old_image_list != NULL)
    {
        BOOL successful = ImageList_Destroy(old_image_list);
        assert(successful);
    }
    current_icon_name = dialog_data->first;
    lv.mask = LVIF_IMAGE | LVIF_TEXT | LVIF_PARAM;
    record_counter = 0;
    while(current_icon_name != NULL)
    {
        if(current_icon_name->image_list_index != IMAGE_LIST_INDEX_INVALID_VALUE)
        {
            WCHAR buffer[15];
            INT index;
            lv.iImage = current_icon_name->image_list_index;
            lv.lParam = record_counter;
            if(current_icon_name->icon_reference_to_index)
            {
                wsprintfW(buffer, mask_integer, current_icon_name->iconReferenceUnion.index);
                lv.pszText = buffer;
            }
            else
                lv.pszText = current_icon_name->iconReferenceUnion.name;
            index = SendMessageW(listview, LVM_INSERTITEMW, 0, (LPARAM) &lv);
            if(index == -1)
            {
                dialog_data->error_code = ERROR_INTERNAL_ERROR;
                return FALSE;
            }
            lv.iItem++;
        }
        current_icon_name = current_icon_name->next;
        record_counter++;
    }
    return TRUE;
}


/* returns false when allocation of memory failed */
static BOOL CALLBACK PickIconDlg_LoadImageNames(HMODULE module, LPCWSTR lpType, LPWSTR name,
    LONG_PTR lparam)
{
    struct tagIconDialogStruct *dialog_data = (struct tagIconDialogStruct *) lparam;
    struct tagIconNamesStruct *icon_name = malloc(sizeof(*icon_name));
    if(icon_name == NULL)
    {
        dialog_data->error_code = ERROR_OUTOFMEMORY;
        return FALSE;
    }
    icon_name->image_list_index = IMAGE_LIST_INDEX_INVALID_VALUE;
    icon_name->icon_reference_to_index = IS_INTRESOURCE(name);
    icon_name->next = NULL;
    if(IS_INTRESOURCE(name))
        icon_name->iconReferenceUnion.index = (INT) (INT_PTR) name;
    else
    {
        icon_name->iconReferenceUnion.name = malloc(sizeof(WCHAR) * (lstrlenW(name) + 1));
        if(icon_name->iconReferenceUnion.name == NULL)
        {
            free(icon_name);
            dialog_data->error_code = ERROR_OUTOFMEMORY;
            return FALSE;
        }
        lstrcpyW(icon_name->iconReferenceUnion.name, name);
    }
    if(dialog_data->first == NULL)
        dialog_data->first = dialog_data->last = icon_name;
    else
    {
        dialog_data->last->next = icon_name;
        dialog_data->last = icon_name;
    }
    return TRUE;
}


static enum enumActionResult PickIconDlg_UpdateImagesDataUseLibrary(const HWND dialog,
    const WCHAR *extended_length_path_filename)
{
    HINSTANCE instance;
    HWND control;
    enum enumActionResult return_value;
    BOOL update_successful;
    struct tagIconDialogStruct *dialog_data = (struct tagIconDialogStruct *)
        GetWindowLongPtrW(dialog, DWLP_USER);
    control = GetDlgItem(dialog, IDC_PICKICONDLG_ICONS);
    instance = LoadLibraryExW(extended_length_path_filename, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if(instance != NULL)
    {
        BOOL successful;
        LVITEMW lv_item;
        return_value = PickIconDlg_ClearIconsData(dialog);
        if(return_value != eDoneSuccessfully)
        {
            successful = FreeLibrary(instance);
            assert(successful);
            return return_value;
        }
        successful = EnumResourceNamesW(instance, (LPCWSTR) RT_GROUP_ICON,
            PickIconDlg_LoadImageNames, (LONG_PTR) dialog_data);
        if(!successful)
        {
            DWORD last_error = GetLastError();
            successful = FreeLibrary(instance);
            assert(successful);
            if((last_error == ERROR_RESOURCE_TYPE_NOT_FOUND) || (last_error == ERROR_RESOURCE_DATA_NOT_FOUND))
                return eDoneSuccessfully;
            dialog_data->error_code = last_error;
            return PickIconDlg_ErrorCodeMeansSeriousError(dialog_data->error_code) ? eNotDoneSeriousError :
                eNotDoneErrorNotSerious;
        }
        SendMessageW(control, WM_SETREDRAW, FALSE, 0);
        update_successful = PickIconDlg_AddIconsToListViewUseLibrary(instance, dialog);
        SendMessageW(control, WM_SETREDRAW, TRUE, 0);
        successful = FreeLibrary(instance);
        assert(successful);
        if(!update_successful)
        {
            return PickIconDlg_ErrorCodeMeansSeriousError(dialog_data->error_code) ? eNotDoneSeriousError :
                eNotDoneErrorNotSerious;
        }
        lv_item.state = lv_item.stateMask = LVIS_FOCUSED | LVIS_SELECTED;
        SendMessageW(control, LVM_SETITEMSTATE, 0, (LPARAM) &lv_item);
        SendMessageW(control, LVM_ENSUREVISIBLE, 0, FALSE);
        return eDoneSuccessfully;
    }
    else
    {
        DWORD last_error = GetLastError();
        if(last_error == ERROR_BAD_EXE_FORMAT)
        {
            return_value = PickIconDlg_ClearIconsData(dialog);
            if(return_value != eDoneSuccessfully)
                return return_value;
            dialog_data->error_code = ERROR_SUCCESS;
            return eDoneSuccessfully;
        }
        else
            dialog_data->error_code = last_error;
        return PickIconDlg_ErrorCodeMeansSeriousError(dialog_data->error_code) ? eNotDoneSeriousError :
            eNotDoneErrorNotSerious;
    }
}


static WCHAR *PickIconDlg_GetInputLine(const HWND dialog, const BOOL also_improve_input_line)
{
    HWND control;
    WCHAR *buffer;
    BOOL changed;
    DWORD length;
    control = GetDlgItem(dialog, IDC_PICKICONDLG_EDIT);
    length = GetWindowTextLengthW(control);
    buffer = malloc((length + 4 + 1) * sizeof(WCHAR));
    if(buffer == NULL)
        return NULL;
    GetWindowTextW(control, &buffer[4], length + 1);
    changed = StrTrimW(&buffer[4], trim_chars);
    length = lstrlenW(&buffer[4]);
    if((length >= MAX_PATH) && also_improve_input_line)
    {
        WCHAR *short_path;
        DWORD length2;
        short_path = malloc((MAX_PATH + 4) * sizeof(WCHAR));
        if(short_path == NULL)
        {
            free(buffer);
            return NULL;
        }
        memmove(buffer, extended_length_path_prefix, 4 * sizeof(WCHAR));
        length2 = GetShortPathNameW(buffer, short_path, MAX_PATH + 4);
        if((length2 != 0) && (length2 - 4 < length))
        {
            lstrcpyW(&buffer[4], &short_path[4]);
            length = length2 - 4;
            changed = TRUE;
        }
        free(short_path);
    }
    memmove(buffer, &buffer[4], length * sizeof(WCHAR) + sizeof(WCHAR));
    if(changed)
        SetWindowTextW(control, buffer);
    return buffer;
}


static enum enumActionResult PickIconDlg_UpdateData(const HWND dialog)
{
    DWORD attributes;
    HWND control, message_dialog_parent;
    WCHAR *input_text, *extended_length_path_filename = NULL;
    HINSTANCE instance;
    enum enumActionResult return_value;
    struct tagIconDialogStruct *dialog_data = (struct tagIconDialogStruct *)
        GetWindowLongPtrW(dialog, DWLP_USER);
    message_dialog_parent = IsWindowVisible(dialog) ? dialog :
        dialog_data->parent_of_pick_icon_dialog;
    input_text = PickIconDlg_GetInputLine(dialog, TRUE);
    if(input_text == NULL)
        return eNotDoneSeriousError;
    if(lstrlenW(input_text) >= MAX_PATH)
    {
        enum enumActionResult result;
        free(input_text);
        result =  PickIconDlg_ClearIconsData(dialog);
        if(result == eDoneSuccessfully)
        {
            dialog_data->error_code = ERROR_PATH_NOT_FOUND;
            return eNotDoneErrorNotSerious;
        }
        else
            return result;
    }
    extended_length_path_filename = PickIconDlg_CreateExpandedOrCompletedPathFileName(input_text, TRUE);
    if(extended_length_path_filename == NULL)
    {
        dialog_data->error_code = ERROR_OUTOFMEMORY;
        free(input_text);
        return eNotDoneSeriousError;
    }
    attributes = GetFileAttributesW(extended_length_path_filename);
    if((attributes == INVALID_FILE_ATTRIBUTES) || ((attributes & FILE_ATTRIBUTE_DIRECTORY) > 0))
    {
        free(extended_length_path_filename);
        instance = (HINSTANCE) GetWindowLongPtrW(dialog, GWLP_HINSTANCE);
        dialog_data->ignore_notifications = TRUE;
        ShellMessageBoxW(instance, message_dialog_parent,
            (WCHAR *) MAKEINTRESOURCE(IDS_PID_FILEDOESNOTEXIST),
            (WCHAR *) MAKEINTRESOURCE(IDS_PID_CHANGEICONPROBLEM), MB_OK | MB_ICONSTOP, input_text);
        dialog_data->ignore_notifications = FALSE;
        free(input_text);
        if(dialog_data->bad_input_error_message_showed)
        {
            enum enumActionResult state = PickIconDlg_ClearIconsData(dialog);
            dialog_data->path_filename_changed_without_data_update = FALSE;
            return state;
        }
        extended_length_path_filename = PickIconDlg_CreateExpandedOrCompletedPathFileName(
            (const WCHAR *) shell32_dll_file_name, TRUE);
        if(extended_length_path_filename == NULL)
        {
            dialog_data->error_code = ERROR_OUTOFMEMORY;
            return eNotDoneSeriousError;
        }
        input_text = PickIconDlg_GetSystemDirectoryPathFilename((const WCHAR *) shell32_dll_file_name,
            FALSE, TRUE);
        if(input_text == NULL)
        {
            dialog_data->error_code = ERROR_OUTOFMEMORY;
            free(extended_length_path_filename);
            return eNotDoneSeriousError;
        }
        dialog_data->bad_input_error_message_showed = TRUE;
        control = GetDlgItem(dialog, IDC_PICKICONDLG_EDIT);
        SetWindowTextW(control, input_text);
    }
    control = GetDlgItem(dialog, IDC_PICKICONDLG_ICONS);
    if(PickIconDlg_IsIcoFile(extended_length_path_filename))
        return_value = PickIconDlg_UpdateImagesDataUseIcoFile(dialog, extended_length_path_filename);
    else
        return_value = PickIconDlg_UpdateImagesDataUseLibrary(dialog, extended_length_path_filename);
    free(extended_length_path_filename);
    if(return_value == eDoneSuccessfully)
    {
        INT items_count = 0;
        INT items_shown = SendMessageW(control, LVM_GETITEMCOUNT, 0, 0);
        struct tagIconNamesStruct *current_record = dialog_data->first;
        while(current_record != NULL)
        {
            current_record = current_record->next;
            items_count++;
        }
        if((items_count == 0) || (items_shown < items_count))
        {
            instance = (HINSTANCE) GetWindowLongPtrW(dialog, GWLP_HINSTANCE);
            dialog_data->ignore_notifications = TRUE;
            if(!items_count)
                ShellMessageBoxW(instance, message_dialog_parent, (WCHAR *)
                    MAKEINTRESOURCE(IDS_PID_FILENOICONS), (WCHAR *)
                    MAKEINTRESOURCE(IDS_PID_CHANGEICONPROBLEM), MB_ICONWARNING | MB_OK, input_text);
            else
                ShellMessageBoxW(instance, message_dialog_parent, (WCHAR *)
                    MAKEINTRESOURCE(IDS_PID_ICONSPARTLYLOADED), (WCHAR *)
                    MAKEINTRESOURCE(IDS_PID_CHANGEICONPROBLEM), MB_OK | MB_ICONINFORMATION,
                    items_shown, items_count, input_text);
            dialog_data->ignore_notifications = FALSE;
        }
    }
    free(input_text);

    dialog_data->path_filename_changed_without_data_update = FALSE;
    return return_value;
}


static VOID PickIconDlg_OnUpdateEditFilename(const HWND dialog)
{
    struct tagIconDialogStruct *dialog_data = (VOID *)
        GetWindowLongPtrW(dialog, DWLP_USER);
    if(dialog_data != NULL)
        dialog_data->path_filename_changed_without_data_update = TRUE;
}


static enum enumActionResult PickIconDlg_OnInit(const HWND dialog,
    struct tagIconDialogStruct *dialog_data)
{
    HWND control;
    BOOL successful;
    enum enumActionResult result;
    SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR) dialog_data);
    dialog_data->first = dialog_data->last = NULL;
    dialog_data->error_code = ERROR_SUCCESS;
    assert(dialog_data->path_filename_max_chars > 0);
    control = GetDlgItem(dialog, IDC_PICKICONDLG_EDIT);
    SendMessageW(control, EM_LIMITTEXT, dialog_data->new_path_filename_max_length, 0);
    StrTrimW(dialog_data->path_filename_allocated, trim_chars);
    if(lstrlenW(dialog_data->path_filename_allocated) == 0)
    {
        WCHAR *new_text_field_value;
        new_text_field_value = PickIconDlg_GetSystemDirectoryPathFilename((const WCHAR *)
            shell32_dll_file_name, FALSE, TRUE);
        if(new_text_field_value == NULL)
        {
            dialog_data->error_code = ERROR_OUTOFMEMORY;
            return eNotDoneSeriousError;
        }
        successful = SetWindowTextW(control, new_text_field_value);
        free(new_text_field_value);
    }
    else
        successful = SetWindowTextW(control, dialog_data->path_filename_allocated);
    assert(successful);
    dialog_data->path_filename_changed_without_data_update = TRUE;
    SendDlgItemMessageW(dialog, IDC_PICKICONDLG_ICONS, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
        LVS_EX_BORDERSELECT
#ifdef LVS_EX_HIDELABELS_IMPLEMENTED
        | LVS_EX_HIDELABELS
#endif
        );
    result = PickIconDlg_UpdateData(dialog);
    if(result == eDoneSuccessfully)
    {
        INT index;
        LVFINDINFOW find_info = { 0 };
        find_info.lParam = dialog_data->icon_index;
        find_info.flags = LVFI_PARAM;
        control = GetDlgItem(dialog, IDC_PICKICONDLG_ICONS);
        index = SendMessageW(control, LVM_FINDITEMW, -1, (LPARAM) &find_info);
        if(index > -1)
        {
            LVITEMW item;
            item.state = item.stateMask = LVNI_SELECTED | LVNI_FOCUSED;
            SendMessageW(control, LVM_SETITEMSTATE, (WPARAM) index, (LPARAM)
                (LPLVITEMW) &item);
            SendMessageW(control, LVM_ENSUREVISIBLE, (WPARAM) index, (LPARAM) TRUE);
        }
    }
    return result;
}


static enum enumActionResult PickIconDlg_UpdatePathFilenameAllocated(const HWND dialog)
{
    WCHAR *input_line;
    enum enumActionResult result = eDoneSuccessfully;
    struct tagIconDialogStruct *dialog_data = (struct tagIconDialogStruct *)
        GetWindowLongPtrW(dialog, DWLP_USER);
    input_line = PickIconDlg_GetInputLine(dialog, FALSE);
    if(input_line == NULL)
    {
        dialog_data->error_code = ERROR_OUTOFMEMORY;
        result = eNotDoneSeriousError;
    }
    if(result == eDoneSuccessfully)
    {
        swprintf(dialog_data->path_filename_allocated, dialog_data->path_filename_max_chars,
            mask_s, input_line);
        free(input_line);
    }
    return result;
}


static enum enumActionResult PickIconDlg_OnConfirmation(const HWND dialog)
{
    HWND control;
    INT icon_count;
    INT index;
    struct tagIconDialogStruct *dialog_data = (struct tagIconDialogStruct *)
        GetWindowLongPtrW(dialog, DWLP_USER);
    if(dialog_data->path_filename_changed_without_data_update == TRUE)
    {
        enum enumActionResult answer = PickIconDlg_UpdateData(dialog);
        return answer;
    }
    icon_count = SendDlgItemMessageW(dialog, IDC_PICKICONDLG_ICONS, LVM_GETITEMCOUNT, 0, 0);
    if(icon_count == 0)
    {
        enum enumActionResult result = PickIconDlg_UpdatePathFilenameAllocated(dialog);
        if(result == eDoneSuccessfully)
        {
            dialog_data->icon_index = 0;
            return eDoneSuccessfullyWithAcceptableConfirmation;
        }
        else
            return result;
    }
    else
    {
        struct tagIconNamesStruct *current_record;
        BOOL successful;
        INT counter = 0;
        LVITEMW item = { 0 };
        control = GetDlgItem(dialog, IDC_PICKICONDLG_ICONS);
        index = SendMessageW(control, LVM_GETNEXTITEM, (WPARAM) -1, (LPARAM) LVNI_FOCUSED);
        if(index == -1)
            return eNotDoneNoError;
        item.cchTextMax = sizeof(item);
        item.mask = LVIF_IMAGE;
        item.iItem = index;
        successful = SendMessageW(control, LVM_GETITEMW, 0, (LPARAM) &item);
        assert(successful);
        current_record = dialog_data->first;
        while(current_record != NULL)
        {
            if(current_record->image_list_index == item.iImage)
            {
                enum enumActionResult result;
                dialog_data->icon_index = counter;
                result = PickIconDlg_ClearIconsData(dialog);
                if(result == eDoneSuccessfully)
                    result = PickIconDlg_UpdatePathFilenameAllocated(dialog);
                if(result == eDoneSuccessfully)
                    return eDoneSuccessfullyWithAcceptableConfirmation;
                else
                    return result;
            }
            current_record = current_record->next;
            counter++;
        }
    }
    return eNotDoneNoError;
}


static enum enumActionResult PickIconDlg_OnClickButton(const HWND dialog)
{
    HWND hwnd;
    OPENFILENAMEW ofn;
    WCHAR *selection_text = NULL;
    HINSTANCE instance;
    DWORD text_length, length;
    WCHAR *input_line_path_filename, *input_line_short_path_filename, *input_line_text;
    enum enumActionResult return_value;
    struct tagIconDialogStruct *dialog_data;

    dialog_data = (struct tagIconDialogStruct *) GetWindowLongPtrW(dialog, DWLP_USER);
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dialog;
    ofn.nMaxFile = MAX_PATH;
    hwnd = GetDlgItem(dialog, IDC_PICKICONDLG_EDIT);
    input_line_text = PickIconDlg_GetInputLine(dialog, FALSE);
    ofn.lpstrFile = malloc(ofn.nMaxFile * sizeof(WCHAR));
    if((input_line_text == NULL) || (ofn.lpstrFile == NULL))
    {
        if(input_line_text != NULL)
            free(input_line_text);
        if(ofn.lpstrFile != NULL)
            free(ofn.lpstrFile);
        dialog_data->error_code = ERROR_OUTOFMEMORY;
        return eNotDoneSeriousError;
    }
    input_line_path_filename = PickIconDlg_CreateExpandedOrCompletedPathFileName(
        input_line_text, TRUE);
    free(input_line_text);
    if(input_line_path_filename == NULL)
    {
        dialog_data->error_code = ERROR_OUTOFMEMORY;
        return  eNotDoneSeriousError;
    }
    if(lstrlenW(&input_line_path_filename[4]) >= MAX_PATH)
    {
        DWORD length_long;
        input_line_short_path_filename = malloc((MAX_PATH + 4) * sizeof(WCHAR));
        if(input_line_short_path_filename == NULL)
        {
            free(input_line_path_filename);
            dialog_data->error_code = ERROR_OUTOFMEMORY;
            return  eNotDoneSeriousError;
        }
        length_long = lstrlenW(input_line_path_filename);
        length = GetShortPathNameW(input_line_path_filename,
            input_line_short_path_filename, MAX_PATH + 4);
        if(length <= 4)
            ofn.lpstrFile[0] = 0;
        else if((length - 4 < ofn.nMaxFile) && (length < length_long))
            lstrcpyW(ofn.lpstrFile, &input_line_short_path_filename[4]);
        free(input_line_short_path_filename);
    }
    else
        lstrcpyW(ofn.lpstrFile, &input_line_path_filename[4]);
    free(input_line_path_filename);
    instance = (HINSTANCE) GetWindowLongPtrW(dialog, GWLP_HINSTANCE);
    text_length = LoadStringW(instance, IDS_PID_ICONFILEEXTENSIONS, (WCHAR *)
        &selection_text, 0);
    assert(text_length > 0);
    ofn.lpstrFilter = selection_text;
    ofn.lpstrTitle = NULL;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if(GetOpenFileNameW(&ofn))
    {
        SetWindowTextW(hwnd, ofn.lpstrFile);
        return_value = PickIconDlg_UpdateData(dialog);
    }
    else
        return_value = eDoneSuccessfully;
    free(ofn.lpstrFile);
    return return_value;
}


static enum enumActionResult PickIconDlg_OnNotify(const HWND dialog, const LPNMHDR lparam)
{
    struct tagIconDialogStruct *dialog_data = (struct tagIconDialogStruct *)
        GetWindowLongPtrW(dialog, DWLP_USER);
    if(dialog_data)
        if(dialog_data->ignore_notifications)
            return eNotDoneNoError;
    switch(lparam->code)
    {
        case NM_DBLCLK:
            return PickIconDlg_OnConfirmation(dialog);
        case NM_SETFOCUS:
            if(dialog_data->path_filename_changed_without_data_update)
                return PickIconDlg_UpdateData(dialog);
        break;
    }
    return eNotDoneNoError;
}


static VOID PickIconDlg_ShowErrorMessageAndClearErrorCode(const HWND dialog,
    struct tagIconDialogStruct *dialog_data, const WORD ids_body_text_mask)
{
    static const WCHAR mask_u[] = { '%', 'u', 0 };
    const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    LPWSTR system_message = NULL;
    WCHAR number[6];
    HINSTANCE instance;
    DWORD chars;
    instance = (HINSTANCE) GetWindowLongPtrW(dialog, GWLP_HINSTANCE);
    chars = FormatMessageW(flags, NULL, dialog_data->error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR) &system_message, 0, NULL);
    if(chars == 0)
    {
        swprintf(number, ARRAYSIZE(number), mask_u, dialog_data->error_code);
        system_message = number;
    }
    dialog_data->ignore_notifications = TRUE;
    ShellMessageBoxW(instance, dialog, (WCHAR *) MAKEINTRESOURCE(ids_body_text_mask), (WCHAR *)
        MAKEINTRESOURCE(IDS_PID_CHANGEICONPROBLEM), MB_OK | MB_ICONSTOP, system_message,
        dialog_data->error_code);
    if(chars > 0)
        LocalFree(system_message);
    dialog_data->ignore_notifications = FALSE;
    dialog_data->error_code = ERROR_SUCCESS;
}


static VOID PickIconDlg_ProcessSuccessState(const HWND dialog, const enum enumActionResult what_happened)
{
    struct tagIconDialogStruct *dialog_data;
    switch(what_happened)
    {
        case eDoneSuccessfullyWithAcceptableConfirmation:
            PickIconDlg_ClearIconsData(dialog);
            EndDialog(dialog, IDOK);
            break;
        case eNotDoneErrorNotSerious:
            dialog_data = (struct tagIconDialogStruct *) GetWindowLongPtrW(dialog, DWLP_USER);
            PickIconDlg_ShowErrorMessageAndClearErrorCode(IsWindowVisible(dialog) ? dialog :
                dialog_data->parent_of_pick_icon_dialog, dialog_data, IDS_PID_PROCESSINGFAILURE);
            break;
        case eNotDoneSeriousError:
            PickIconDlg_ClearIconsData(dialog);
            EndDialog(dialog, IDCANCEL);
            break;
        default:
            ;
    }
}


static INT_PTR CALLBACK PickIconDlg_Proc(HWND dialog, UINT message,
     WPARAM wparam, LPARAM lparam)
{
    struct tagIconDialogStruct *dialog_data;
    enum enumActionResult success_state;
    switch(message)
    {
        case WM_INITDIALOG:
            dialog_data = (struct tagIconDialogStruct *) lparam;
            success_state = PickIconDlg_OnInit(dialog, dialog_data);
            PickIconDlg_ProcessSuccessState(dialog, success_state);
            return TRUE;
        case WM_COMMAND:
            switch(LOWORD(wparam))
            {
            case IDOK:
                success_state = PickIconDlg_OnConfirmation(dialog);
                PickIconDlg_ProcessSuccessState(dialog, success_state);
                break;
            case IDCANCEL:
                PickIconDlg_ClearIconsData(dialog);
                EndDialog(dialog, IDCANCEL);
                break;
            case IDC_PICKICONDLG_EDIT:
                if(HIWORD(wparam) == EN_UPDATE)
                    PickIconDlg_OnUpdateEditFilename(dialog);
                break;
            case IDC_PICKICONDLG_CHANGE:
                if(HIWORD(wparam) == BN_CLICKED)
                {
                    success_state = PickIconDlg_OnClickButton(dialog);
                    PickIconDlg_ProcessSuccessState(dialog, success_state);
                }
            }
            break;
        case WM_NOTIFY:
            success_state = PickIconDlg_OnNotify(dialog, (LPNMHDR) lparam);
            PickIconDlg_ProcessSuccessState(dialog, success_state);
            break;
        default:
            return FALSE;
        }
    return TRUE;
}


/*************************************************************************
 * PickIconDlg					[SHELL32.62]
 *
 */
INT WINAPI PickIconDlg(HWND hwnd_owner, WCHAR *path, UINT path_len, INT *index)
{
    size_t needed_bytes, needed_bytes2;
    HINSTANCE instance;
    BOOL icon_selected = FALSE;
    INITCOMMONCONTROLSEX comm_ctls_ex = { sizeof(comm_ctls_ex), ICC_LISTVIEW_CLASSES };
    struct tagIconDialogStruct dialog_data = { 0 };
    if((path == NULL) || (index == NULL))
        return 0;
    InitCommonControlsEx(&comm_ctls_ex);
    instance = shell32_hInstance;
    if(hwnd_owner != NULL)
    {
        if(!IsWindow(hwnd_owner))
        {
            SetLastError(ERROR_INVALID_WINDOW_HANDLE);
            /* Here, an implementation that does not correspond to the documentation is
             * being reprogrammed. */
            return 1;
        }
    }
    dialog_data.parent_of_pick_icon_dialog = hwnd_owner;
    if(path_len < 2)
        dialog_data.new_path_filename_max_length = 1;
    else
        dialog_data.new_path_filename_max_length = path_len - 1;
    needed_bytes = sizeof(WCHAR) * (dialog_data.new_path_filename_max_length + 1);
    needed_bytes2 = (lstrlenW(path) + 1) * sizeof(WCHAR);
    if(needed_bytes < needed_bytes2)
        needed_bytes = needed_bytes2;
    dialog_data.path_filename_allocated = malloc(needed_bytes);
    if(dialog_data.path_filename_allocated == NULL)
        dialog_data.error_code = ERROR_OUTOFMEMORY;
    else
        dialog_data.error_code = ERROR_SUCCESS;
    if(dialog_data.error_code == ERROR_SUCCESS)
    {
        lstrcpyW(dialog_data.path_filename_allocated, path);
        dialog_data.icon_index = *index;
        dialog_data.path_filename_max_chars = needed_bytes / sizeof(WCHAR);
        icon_selected = DialogBoxParamW(instance, (WCHAR *) MAKEINTRESOURCE(IDD_PICKICONDLG), hwnd_owner,
            PickIconDlg_Proc, (LPARAM) &dialog_data) == IDOK;
    }
    if(dialog_data.error_code == ERROR_SUCCESS)
    {
        if(icon_selected)
        {
            *index = dialog_data.icon_index;
            swprintf(path, path_len, mask_s, dialog_data.path_filename_allocated);
        }
    }
    else
    {
        WCHAR *system_message = NULL;
        const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER;
        FormatMessageW(flags, NULL, dialog_data.error_code,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (WCHAR *) &system_message, 0, NULL);
        ShellMessageBoxW(instance, hwnd_owner, (WCHAR *) MAKEINTRESOURCE(IDS_PID_DIALOGCOULDNOTBEDISPLAYED),
            (WCHAR *) MAKEINTRESOURCE(IDS_PID_CHANGEICONPROBLEM), MB_OK | MB_ICONSTOP, system_message);
        LocalFree(system_message);
    }
    if(dialog_data.path_filename_allocated != NULL)
        free(dialog_data.path_filename_allocated);
    SetLastError(dialog_data.error_code);
    return icon_selected ? 1 : 0;
}


HRESULT WINAPI SHOpenWithDialog(HWND parent, const OPENASINFO *info)
{
    FIXME("stub\n");
    return E_NOTIMPL;
}

/*************************************************************************
 * RunFileDlgW					[internal]
 *
 * The Unicode function that is available as ordinal 61 on Windows NT/2000/XP/...
 *
 * SEE ALSO
 *   RunFileDlgAW
 */
static void RunFileDlgW(
	HWND hwndOwner,
	HICON hIcon,
	LPCWSTR lpstrDirectory,
	LPCWSTR lpstrTitle,
	LPCWSTR lpstrDescription,
	UINT uFlags)
{
    RUNFILEDLGPARAMS rfdp;
    HRSRC hRes;
    LPVOID template;
    TRACE("\n");

    rfdp.hwndOwner        = hwndOwner;
    rfdp.hIcon            = hIcon;
    rfdp.lpstrDirectory   = lpstrDirectory;
    rfdp.lpstrTitle       = lpstrTitle;
    rfdp.lpstrDescription = lpstrDescription;
    rfdp.uFlags           = uFlags;

    if (!(hRes = FindResourceW(shell32_hInstance, L"SHELL_RUN_DLG", (LPWSTR)RT_DIALOG)) ||
        !(template = LoadResource(shell32_hInstance, hRes)))
    {
        ERR("Couldn't load SHELL_RUN_DLG resource\n");
        ShellMessageBoxW(shell32_hInstance, hwndOwner, MAKEINTRESOURCEW(IDS_RUNDLG_ERROR), NULL, MB_OK | MB_ICONERROR);
        return;
    }

    DialogBoxIndirectParamW(shell32_hInstance,
			    template, hwndOwner, RunDlgProc, (LPARAM)&rfdp);

}

/* find the directory that contains the file being run */
static LPWSTR RunDlg_GetParentDir(LPCWSTR cmdline)
{
    const WCHAR *src;
    WCHAR *dest, *result, *result_end=NULL;

    result = malloc(sizeof(WCHAR) * (wcslen(cmdline) + 5));

    src = cmdline;
    dest = result;

    if (*src == '"')
    {
        src++;
        while (*src && *src != '"')
        {
            if (*src == '\\')
                result_end = dest;
            *dest++ = *src++;
        }
    }
    else {
        while (*src)
        {
            if (iswspace(*src))
            {
                *dest = 0;
                if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(result))
                    break;
                lstrcatW(dest, L".exe");
                if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(result))
                    break;
            }
            else if (*src == '\\')
                result_end = dest;
            *dest++ = *src++;
        }
    }

    if (result_end)
    {
        *result_end = 0;
        return result;
    }
    else
    {
        free(result);
        return NULL;
    }
}

/* Dialog procedure for RunFileDlg */
static INT_PTR CALLBACK RunDlgProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
    RUNFILEDLGPARAMS *prfdp = (RUNFILEDLGPARAMS *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (message)
        {
        case WM_INITDIALOG :
            prfdp = (RUNFILEDLGPARAMS *)lParam ;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)prfdp);

            if (prfdp->lpstrTitle)
                SetWindowTextW(hwnd, prfdp->lpstrTitle);
            if (prfdp->lpstrDescription)
                SetWindowTextW(GetDlgItem(hwnd, IDC_RUNDLG_DESCRIPTION), prfdp->lpstrDescription);
            if (prfdp->uFlags & RFF_NOBROWSE)
            {
                HWND browse = GetDlgItem(hwnd, IDC_RUNDLG_BROWSE);
                ShowWindow(browse, SW_HIDE);
                EnableWindow(browse, FALSE);
            }
            if (prfdp->uFlags & RFF_NOLABEL)
                ShowWindow(GetDlgItem(hwnd, IDC_RUNDLG_LABEL), SW_HIDE);
            if (prfdp->uFlags & RFF_CALCDIRECTORY)
                FIXME("RFF_CALCDIRECTORY not supported\n");

            if (prfdp->hIcon == NULL)
                prfdp->hIcon = LoadIconW(NULL, (LPCWSTR)IDI_WINLOGO);
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)prfdp->hIcon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)prfdp->hIcon);
            SendMessageW(GetDlgItem(hwnd, IDC_RUNDLG_ICON), STM_SETICON, (WPARAM)prfdp->hIcon, 0);

            FillList (GetDlgItem (hwnd, IDC_RUNDLG_EDITPATH), NULL, (prfdp->uFlags & RFF_NODEFAULT) == 0) ;
            SetFocus (GetDlgItem (hwnd, IDC_RUNDLG_EDITPATH)) ;
            return TRUE ;

        case WM_COMMAND :
            switch (LOWORD (wParam))
                {
                case IDOK :
                    {
                    int ic ;
                    HWND htxt = GetDlgItem (hwnd, IDC_RUNDLG_EDITPATH);
                    if ((ic = GetWindowTextLengthW (htxt)))
                        {
                        WCHAR *psz, *parent=NULL ;
                        SHELLEXECUTEINFOW sei ;

                        ZeroMemory (&sei, sizeof(sei)) ;
                        sei.cbSize = sizeof(sei) ;
                        psz = malloc( (ic + 1) * sizeof(WCHAR) );
                        GetWindowTextW (htxt, psz, ic + 1) ;

                        /* according to http://www.codeproject.com/KB/shell/runfiledlg.aspx we should send a
                         * WM_NOTIFY before execution */

                        sei.hwnd = hwnd;
                        sei.nShow = SW_SHOWNORMAL;
                        sei.lpFile = psz;

                        if (prfdp->lpstrDirectory)
                            sei.lpDirectory = prfdp->lpstrDirectory;
                        else
                            sei.lpDirectory = parent = RunDlg_GetParentDir(sei.lpFile);

                        if (!ShellExecuteExW( &sei ))
                        {
                            free(psz);
                            free(parent);
                            SendMessageA (htxt, CB_SETEDITSEL, 0, MAKELPARAM (0, -1)) ;
                            return TRUE ;
                        }

                        /* FillList is still ANSI */
                        GetWindowTextA (htxt, (LPSTR)psz, ic + 1) ;
                        FillList (htxt, (LPSTR)psz, FALSE) ;

                        free(psz);
                        free(parent);
                        EndDialog (hwnd, 0);
                        }
                    }
                    return TRUE;

                case IDCANCEL :
                    EndDialog (hwnd, 0) ;
                    return TRUE ;

                case IDC_RUNDLG_BROWSE :
                    {
                    WCHAR szFName[1024] = {0};
                    WCHAR filter_exe[256], filter_all[256], filter[MAX_PATH], szCaption[MAX_PATH];
                    OPENFILENAMEW ofn;

                    LoadStringW(shell32_hInstance, IDS_RUNDLG_BROWSE_FILTER_EXE, filter_exe, 256);
                    LoadStringW(shell32_hInstance, IDS_RUNDLG_BROWSE_FILTER_ALL, filter_all, 256);
                    LoadStringW(shell32_hInstance, IDS_RUNDLG_BROWSE_CAPTION, szCaption, MAX_PATH);
                    swprintf( filter, MAX_PATH, L"%s%c*.exe%c%s%c*.*%c", filter_exe, 0, 0, filter_all, 0, 0 );

                    ZeroMemory(&ofn, sizeof(ofn));
                    ofn.lStructSize = sizeof(OPENFILENAMEW);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = filter;
                    ofn.lpstrFile = szFName;
                    ofn.nMaxFile = 1023;
                    ofn.lpstrTitle = szCaption;
                    ofn.Flags = OFN_ENABLESIZING | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
                    ofn.lpstrInitialDir = prfdp->lpstrDirectory;

                    if (GetOpenFileNameW(&ofn))
                    {
                        SetFocus (GetDlgItem (hwnd, IDOK)) ;
                        SetWindowTextW (GetDlgItem (hwnd, IDC_RUNDLG_EDITPATH), szFName) ;
                        SendMessageW (GetDlgItem (hwnd, IDC_RUNDLG_EDITPATH), CB_SETEDITSEL, 0, MAKELPARAM (0, -1)) ;
                        SetFocus (GetDlgItem (hwnd, IDOK)) ;
                    }

                    return TRUE ;
                    }
                }
            return TRUE ;
        }
    return FALSE ;
    }

/* This grabs the MRU list from the registry and fills the combo for the "Run" dialog above */
/* fShowDefault ignored if pszLatest != NULL */
static void FillList (HWND hCb, char *pszLatest, BOOL fShowDefault)
    {
    HKEY hkey ;
/*    char szDbgMsg[256] = "" ; */
    char *pszList = NULL, *pszCmd = NULL, cMatch = 0, cMax = 0x60, szIndex[2] = "-" ;
    DWORD icList = 0, icCmd = 0 ;
    UINT Nix ;

    SendMessageA (hCb, CB_RESETCONTENT, 0, 0) ;

    if (ERROR_SUCCESS != RegCreateKeyExA (
        HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RunMRU",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hkey, NULL))
        MessageBoxA (hCb, "Unable to open registry key !", "Nix", MB_OK) ;

    RegQueryValueExA (hkey, "MRUList", NULL, NULL, NULL, &icList) ;

    if (icList > 0)
        {
        pszList = malloc(icList) ;
        if (ERROR_SUCCESS != RegQueryValueExA (hkey, "MRUList", NULL, NULL, (LPBYTE)pszList, &icList))
            MessageBoxA (hCb, "Unable to grab MRUList !", "Nix", MB_OK) ;
        }
    else
        {
        icList = 1 ;
        pszList = malloc(icList) ;
        pszList[0] = 0 ;
        }

    for (Nix = 0 ; Nix < icList - 1 ; Nix++)
        {
        if (pszList[Nix] > cMax)
            cMax = pszList[Nix] ;

        szIndex[0] = pszList[Nix] ;

        if (ERROR_SUCCESS != RegQueryValueExA (hkey, szIndex, NULL, NULL, NULL, &icCmd))
            MessageBoxA (hCb, "Unable to grab size of index", "Nix", MB_OK) ;
        pszCmd = realloc(pszCmd, icCmd) ;
        if (ERROR_SUCCESS != RegQueryValueExA (hkey, szIndex, NULL, NULL, (LPBYTE)pszCmd, &icCmd))
            MessageBoxA (hCb, "Unable to grab index", "Nix", MB_OK) ;

        if (NULL != pszLatest)
            {
            if (!lstrcmpiA(pszCmd, pszLatest))
                {
                /*
                sprintf (szDbgMsg, "Found existing (%d).\n", Nix) ;
                MessageBoxA (hCb, szDbgMsg, "Nix", MB_OK) ;
                */
                SendMessageA (hCb, CB_INSERTSTRING, 0, (LPARAM)pszCmd) ;
                SetWindowTextA (hCb, pszCmd) ;
                SendMessageA (hCb, CB_SETEDITSEL, 0, MAKELPARAM (0, -1)) ;

                cMatch = pszList[Nix] ;
                memmove (&pszList[1], pszList, Nix) ;
                pszList[0] = cMatch ;
                continue ;
                }
            }

        if (26 != icList - 1 || icList - 2 != Nix || cMatch || NULL == pszLatest)
            {
            /*
            sprintf (szDbgMsg, "Happily appending (%d).\n", Nix) ;
            MessageBoxA (hCb, szDbgMsg, "Nix", MB_OK) ;
            */
            SendMessageA (hCb, CB_ADDSTRING, 0, (LPARAM)pszCmd) ;
            if (!Nix && fShowDefault)
                {
                SetWindowTextA (hCb, pszCmd) ;
                SendMessageA (hCb, CB_SETEDITSEL, 0, MAKELPARAM (0, -1)) ;
                }

            }
        else
            {
            /*
            sprintf (szDbgMsg, "Doing loop thing.\n") ;
            MessageBoxA (hCb, szDbgMsg, "Nix", MB_OK) ;
            */
            SendMessageA (hCb, CB_INSERTSTRING, 0, (LPARAM)pszLatest) ;
            SetWindowTextA (hCb, pszLatest) ;
            SendMessageA (hCb, CB_SETEDITSEL, 0, MAKELPARAM (0, -1)) ;

            cMatch = pszList[Nix] ;
            memmove (&pszList[1], pszList, Nix) ;
            pszList[0] = cMatch ;
            szIndex[0] = cMatch ;
            RegSetValueExA (hkey, szIndex, 0, REG_SZ, (LPBYTE)pszLatest, strlen (pszLatest) + 1) ;
            }
        }

    if (!cMatch && NULL != pszLatest)
        {
        /*
        sprintf (szDbgMsg, "Simply inserting (increasing list).\n") ;
        MessageBoxA (hCb, szDbgMsg, "Nix", MB_OK) ;
        */
        SendMessageA (hCb, CB_INSERTSTRING, 0, (LPARAM)pszLatest) ;
        SetWindowTextA (hCb, pszLatest) ;
        SendMessageA (hCb, CB_SETEDITSEL, 0, MAKELPARAM (0, -1)) ;

        cMatch = ++cMax ;
        pszList = realloc(pszList, ++icList) ;
        memmove (&pszList[1], pszList, icList - 1) ;
        pszList[0] = cMatch ;
        szIndex[0] = cMatch ;
        RegSetValueExA (hkey, szIndex, 0, REG_SZ, (LPBYTE)pszLatest, strlen (pszLatest) + 1) ;
        }

    RegSetValueExA (hkey, "MRUList", 0, REG_SZ, (LPBYTE)pszList, strlen (pszList) + 1) ;

    free(pszCmd) ;
    free(pszList) ;
}

/*************************************************************************
 * RunFileDlgA					[internal]
 *
 * The ANSI function that is available as ordinal 61 on Windows 9x/Me
 *
 * SEE ALSO
 *   RunFileDlgAW
 */
static void RunFileDlgA(
	HWND hwndOwner,
	HICON hIcon,
	LPCSTR lpstrDirectory,
	LPCSTR lpstrTitle,
	LPCSTR lpstrDescription,
	UINT uFlags)
{
    WCHAR title[MAX_PATH];       /* longer string wouldn't be visible in the dialog anyway */
    WCHAR description[MAX_PATH];
    WCHAR directory[MAX_PATH];

    MultiByteToWideChar(CP_ACP, 0, lpstrTitle, -1, title, MAX_PATH);
    title[MAX_PATH - 1] = 0;
    MultiByteToWideChar(CP_ACP, 0, lpstrDescription, -1, description, MAX_PATH);
    description[MAX_PATH - 1] = 0;
    if (!MultiByteToWideChar(CP_ACP, 0, lpstrDirectory, -1, directory, MAX_PATH))
        directory[0] = 0;

    RunFileDlgW(hwndOwner, hIcon,
        lpstrDirectory ? directory : NULL,
        lpstrTitle ? title : NULL,
        lpstrDescription ? description : NULL,
        uFlags);
}

/*************************************************************************
 * RunFileDlgAW					[SHELL32.61]
 *
 * An undocumented way to open the Run File dialog. A documented way is to use
 * CLSID_Shell, IID_IShellDispatch (as of Wine 1.0, not implemented under Wine)
 *
 * Exported by ordinal. ANSI on Windows 9x and Unicode on Windows NT/2000/XP/etc
 *
 */
void WINAPI RunFileDlgAW(
	HWND hwndOwner,
	HICON hIcon,
	LPCVOID lpstrDirectory,
	LPCVOID lpstrTitle,
	LPCVOID lpstrDescription,
	UINT uFlags)
{
    if (SHELL_OsIsUnicode())
        RunFileDlgW(hwndOwner, hIcon, lpstrDirectory, lpstrTitle, lpstrDescription, uFlags);
    else
        RunFileDlgA(hwndOwner, hIcon, lpstrDirectory, lpstrTitle, lpstrDescription, uFlags);
}


/*************************************************************************
 * ConfirmDialog				[internal]
 *
 * Put up a confirm box, return TRUE if the user confirmed
 */
static BOOL ConfirmDialog(HWND hWndOwner, UINT PromptId, UINT TitleId)
{
  WCHAR Prompt[256];
  WCHAR Title[256];

  LoadStringW(shell32_hInstance, PromptId, Prompt, ARRAY_SIZE(Prompt));
  LoadStringW(shell32_hInstance, TitleId, Title, ARRAY_SIZE(Title));
  return MessageBoxW(hWndOwner, Prompt, Title, MB_YESNO|MB_ICONQUESTION) == IDYES;
}


/*************************************************************************
 * RestartDialogEx				[SHELL32.730]
 */

int WINAPI RestartDialogEx(HWND hWndOwner, LPCWSTR lpwstrReason, DWORD uFlags, DWORD uReason)
{
    TRACE("(%p)\n", hWndOwner);

    /* FIXME: use lpwstrReason */
    if (ConfirmDialog(hWndOwner, IDS_RESTART_PROMPT, IDS_RESTART_TITLE))
    {
        HANDLE hToken;
        TOKEN_PRIVILEGES npr;

        /* enable the shutdown privilege for the current process */
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
        {
            LookupPrivilegeValueA(0, "SeShutdownPrivilege", &npr.Privileges[0].Luid);
            npr.PrivilegeCount = 1;
            npr.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &npr, 0, 0, 0);
            CloseHandle(hToken);
        }
        ExitWindowsEx(EWX_REBOOT, uReason);
    }

    return 0;
}


/*************************************************************************
 * RestartDialog				[SHELL32.59]
 */

int WINAPI RestartDialog(HWND hWndOwner, LPCWSTR lpstrReason, DWORD uFlags)
{
    return RestartDialogEx(hWndOwner, lpstrReason, uFlags, 0);
}


/*************************************************************************
 * ExitWindowsDialog				[SHELL32.60]
 *
 * NOTES
 *     exported by ordinal
 */
void WINAPI ExitWindowsDialog (HWND hWndOwner)
{
    TRACE("(%p)\n", hWndOwner);

    if (ConfirmDialog(hWndOwner, IDS_SHUTDOWN_PROMPT, IDS_SHUTDOWN_TITLE))
    {
        HANDLE hToken;
        TOKEN_PRIVILEGES npr;

        /* enable shutdown privilege for current process */
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
        {
            LookupPrivilegeValueA(0, "SeShutdownPrivilege", &npr.Privileges[0].Luid);
            npr.PrivilegeCount = 1;
            npr.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &npr, 0, 0, 0);
            CloseHandle(hToken);
        }
        ExitWindowsEx(EWX_SHUTDOWN, 0);
    }
}
