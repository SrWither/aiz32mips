// stdio.h — E/S mínima. La consola es el único "archivo" que hay por
// ahora: no existe syscall de FS para userland todavía (fs.c lo usa la
// shell directo desde contexto kernel, ver kernel/shell.c) — eso llega
// cuando haga falta abrir archivos desde un programa de usuario, no antes.
// getchar no bloquea (ver sys_getc en abi.h): devuelve -1 si no hay tecla
// lista, no espera a que la haya.
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
