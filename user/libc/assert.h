// assert.h — chequeo mínimo estilo <assert.h>: sin __FILE__/__LINE__ de
// verdad todavía (no cuesta agregarlos, pero nada de este proyecto los
// pidió), imprime la condición y corta el proceso vía exit(1). NDEBUG lo
// apaga por completo, igual que el assert() real.
#ifndef AIZ_LIBC_ASSERT_H
#define AIZ_LIBC_ASSERT_H

#include "stdio.h"
#include "stdlib.h"

#ifdef NDEBUG
#define assert(cond) ((void)0)
#else
#define assert(cond)                                    \
    do {                                                \
        if (!(cond)) {                                  \
            printf("assert fallado: %s\n", #cond);       \
            exit(1);                                     \
        }                                                \
    } while (0)
#endif

#endif // AIZ_LIBC_ASSERT_H
