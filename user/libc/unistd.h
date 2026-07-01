// unistd.h — fork() sobre sys_fork() (ver kernel/trap.c y
// kernel/sched.c::sched_fork). Copia completa de las 4 páginas del
// proceso, no copy-on-write: cada fork() cuesta duplicar 16KB.
#ifndef AIZ_LIBC_UNISTD_H
#define AIZ_LIBC_UNISTD_H

#include "../../kernel/abi.h"

static inline int fork(void) {
    return sys_fork();
}

#endif // AIZ_LIBC_UNISTD_H
