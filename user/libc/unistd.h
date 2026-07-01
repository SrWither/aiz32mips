// unistd.h — fork() sobre sys_fork() (ver kernel/trap.c y
// kernel/sched.c::sched_fork). Copia completa de las 4 páginas del
// proceso, no copy-on-write: cada fork() cuesta duplicar 16KB.
//
// open/read/write/close sobre las syscalls de archivo (ver kernel/fs.c):
// fd >= 3, sin stdin/stdout/stderr todavía (la consola sigue yendo por
// sys_putc/sys_getc, ver libc/stdio.h — son caminos separados por ahora).
// write() no persiste nada hasta close(): se acumula en un buffer del
// kernel (FS_WRITE_BUF_SIZE, 4KB) y recién ahí se escribe a FAT32 de una.
#ifndef AIZ_LIBC_UNISTD_H
#define AIZ_LIBC_UNISTD_H

#include "../../kernel/abi.h"

#define O_RDONLY 0
#define O_WRONLY 1

static inline int fork(void) {
    return sys_fork();
}

static inline int open(const char *path, int flags) {
    return sys_open(path, flags);
}

static inline int read(int fd, void *buf, unsigned int count) {
    return sys_read(fd, buf, count);
}

static inline int write(int fd, const void *buf, unsigned int count) {
    return sys_write(fd, buf, count);
}

static inline int close(int fd) {
    return sys_close(fd);
}

#endif // AIZ_LIBC_UNISTD_H
