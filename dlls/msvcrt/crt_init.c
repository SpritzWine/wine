/* Utility functions for calling MSVC/MingW ctors & dtors from mainCRTStartup. */

#if 0
#pragma makedep implib
#endif

#include <process.h>
#include <stdlib.h>
#include "minwindef.h"

/* Reference: mingw-w64-crt: crt/gccmain.c */
typedef void (*fnptr)(void);
extern fnptr __CTOR_LIST__[];
extern fnptr __DTOR_LIST__[];

extern _PIFV __xi_a[];
extern _PIFV __xi_z[];
extern _PVFV __xc_a[];
extern _PVFV __xc_z[];
extern _PVFV __xt_a[];
extern _PVFV __xt_z[];

static inline int INIT_initterm_e(_PIFV *table, _PIFV *end)
{
#if _MSVCR_VER<80
    int res = 0;

    while (!res && table < end) {
        if (*table)
            res = (**table)();
        table++;
    }
    return res;
#else
    return _initterm_e(table, end);
#endif
}

void CDECL _initterm(_PVFV *,_PVFV *);

static __cdecl void do_global_dtors(void)
{
    SIZE_T i;
    for (i = 1; __DTOR_LIST__[i]; i++) __DTOR_LIST__[i]();
    _initterm(__xt_a, __xt_z);
}

void do_global_ctors(void)
{
    ULONG_PTR nfns = (ULONG_PTR)__CTOR_LIST__[0], i;
    if (nfns == (ULONG_PTR)-1)
        for (nfns = 0; __CTOR_LIST__[nfns + 1]; nfns++);
    for (i = nfns; i >= 1; i--) __CTOR_LIST__[i]();

    if (INIT_initterm_e(__xi_a, __xi_z) != 0) return;
    _initterm(__xc_a, __xc_z);

#ifdef _UCRT
    _crt_atexit(do_global_dtors);
#else
    _onexit((_onexit_t)do_global_dtors);
#endif
}
