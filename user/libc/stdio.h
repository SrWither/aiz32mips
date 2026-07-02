// stdio.h — E/S mínima: consola (sys_putc/sys_getc, sin buffering) +
// FILE* sobre las syscalls de archivo de fd crudo (SYS_OPEN/READ/WRITE/
// CLOSE/LSEEK, ver kernel/fs.c y libc/unistd.h para la versión sin
// envoltorio). getchar no bloquea (ver sys_getc en abi.h): devuelve -1 si
// no hay tecla lista, no espera a que la haya.
#ifndef AIZ_LIBC_STDIO_H
#define AIZ_LIBC_STDIO_H

#ifndef AIZ_GPU_TYPES_DEFINED
#define AIZ_GPU_TYPES_DEFINED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
#endif

#include "../../kernel/abi.h"
#include <stdarg.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EOF (-1)

// FILE: envoltorio de un fd (ver kernel/fs.c) + flags de estado. Nada de
// buffering propio (cada fread/fwrite es un syscall directo) — este
// proyecto no tiene stdio bufferizado todavía, alcanza para lo que se
// necesita (WAD de DOOM, tests de FS).
typedef struct {
    int fd;
    int eof;
    int error;
} FILE;

// Pool estático en vez de malloc: fopen no debería depender de que el heap
// (stdlib.h) esté inicializado. fd==0 marca un slot libre (FD_BASE=3 en
// fs.c, así que 0 nunca es un fd real).
#define FOPEN_MAX 8
static FILE _file_pool[FOPEN_MAX];

// mode: sólo importa el primer caracter, 'w' es escritura (crea/trunca,
// ver fs_open/fs_fd_close), cualquier otra cosa ('r', "rb", etc.) es
// lectura. Sin modo "append" ni "r+" — mismo alcance que sys_open.
static inline FILE *fopen(const char *path, const char *mode) {
    int write_mode = (mode[0] == 'w') ? 1 : 0;
    int fd = sys_open(path, write_mode);
    if (fd < 0) {
        return NULL;
    }
    for (int i = 0; i < FOPEN_MAX; i++) {
        if (_file_pool[i].fd == 0) {
            _file_pool[i].fd = fd;
            _file_pool[i].eof = 0;
            _file_pool[i].error = 0;
            return &_file_pool[i];
        }
    }
    sys_close(fd); // sin slot libre en el pool: no dejar el fd huérfano
    return NULL;
}

static inline u32 fread(void *ptr, u32 size, u32 nmemb, FILE *f) {
    u32 total = size * nmemb;
    int n = sys_read(f->fd, ptr, total);
    if (n < 0) {
        f->error = 1;
        return 0;
    }
    if ((u32)n < total) {
        f->eof = 1; // simplificado: no distingue "llegué justo al final" de "pedí de más"
    }
    return size ? (u32)n / size : 0;
}

static inline u32 fwrite(const void *ptr, u32 size, u32 nmemb, FILE *f) {
    u32 total = size * nmemb;
    int n = sys_write(f->fd, ptr, total);
    if (n < 0) {
        f->error = 1;
        return 0;
    }
    return size ? (u32)n / size : 0;
}

static inline int fseek(FILE *f, int offset, int whence) {
    return sys_lseek(f->fd, offset, whence) < 0 ? -1 : 0;
}

static inline int ftell(FILE *f) {
    return sys_lseek(f->fd, 0, SEEK_CUR);
}

static inline int feof(FILE *f) {
    return f->eof;
}

static inline int ferror(FILE *f) {
    return f->error;
}

static inline int fclose(FILE *f) {
    int r = sys_close(f->fd);
    f->fd = 0; // libera el slot del pool
    return r;
}

static inline int putchar(int c) {
    sys_putc((char)c);
    return c;
}

static inline int puts(const char *s) {
    while (*s) {
        sys_putc(*s++);
    }
    sys_putc('\n');
    return 0;
}

static inline int getchar(void) {
    return sys_getc();
}

static inline void print_uint(u32 v, u32 base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[12];
    int n = 0;
    if (v == 0) {
        putchar('0');
        return;
    }
    while (v > 0) {
        buf[n++] = digits[v % base];
        v /= base;
    }
    while (n > 0) {
        putchar(buf[--n]);
    }
}

static inline void print_int(int v) {
    if (v < 0) {
        putchar('-');
        print_uint((u32)(-v), 10, 0);
    } else {
        print_uint((u32)v, 10, 0);
    }
}

// printf mínimo: %d %u %x %X %c %s %%. Sin ancho, precisión ni flags —
// alcanza para debug/logging; se amplía el día que haga falta más.
static inline int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            putchar(*p);
            continue;
        }
        p++;
        switch (*p) {
            case 'd':
                print_int(va_arg(ap, int));
                break;
            case 'u':
                print_uint(va_arg(ap, unsigned int), 10, 0);
                break;
            case 'x':
                print_uint(va_arg(ap, unsigned int), 16, 0);
                break;
            case 'X':
                print_uint(va_arg(ap, unsigned int), 16, 1);
                break;
            case 'c':
                putchar(va_arg(ap, int));
                break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                while (*s) {
                    putchar(*s++);
                }
                break;
            }
            case '%':
                putchar('%');
                break;
            case 0:
                p--; // "%" suelto al final de la cadena: no comerse el \0
                break;
            default:
                putchar('%');
                putchar(*p);
                break;
        }
    }
    va_end(ap);
    return 0;
}

#endif // AIZ_LIBC_STDIO_H
