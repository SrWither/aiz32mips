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
#include "string.h" // fputs necesita strlen
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

static inline int fputc(int c, FILE *f) {
    char ch = (char)c;
    return (fwrite(&ch, 1, 1, f) == 1) ? c : EOF;
}

static inline int fputs(const char *s, FILE *f) {
    u32 len = strlen(s);
    return (fwrite(s, 1, len, f) == len) ? 0 : EOF;
}

// Lee hasta `size-1` bytes o hasta '\n' (inclusive) o EOF, lo que pase
// primero — sin buffering propio (cada fread es un syscall directo, mismo
// criterio que el resto de este archivo), así que esto es tan lento como
// un fread() por carácter, pero alcanza para leer líneas de un archivo
// chico (WAD, configs). NULL si no se leyó nada (EOF inmediato).
static inline char *fgets(char *buf, int size, FILE *f) {
    if (size <= 0) {
        return NULL;
    }
    int i = 0;
    while (i < size - 1) {
        char c;
        if (fread(&c, 1, 1, f) == 0) {
            break;
        }
        buf[i++] = c;
        if (c == '\n') {
            break;
        }
    }
    buf[i] = 0;
    return (i == 0) ? NULL : buf;
}

// ────────────────────── printf/snprintf/fprintf ────────────────────────
// Un solo motor de formato (vformat) sobre un "sink" que puede ser la
// consola, un buffer acotado (snprintf) o un FILE* (fprintf) — antes
// printf tenía su propio loop duplicado con print_uint/print_int; ahora
// las 3 variantes comparten el mismo parser de "%d %u %x %X %c %s %%".
// Sin ancho, precisión ni flags — alcanza para debug/logging y armar
// strings chicos; se amplía el día que haga falta más.
typedef struct {
    char *buf;  // NULL (con file==NULL) => sink de consola, vía putchar
    u32 pos;    // caracteres "escritos" hasta ahora, cuenten o no en buf
                // (mismo contrato que el snprintf real: el valor de
                // retorno es cuánto se HABRÍA escrito, truncado o no)
    u32 cap;    // tamaño de buf; ignorado si buf==NULL
    FILE *file; // no-NULL => sink de archivo, vía fwrite (fprintf)
} fmt_sink_t;

static inline void fmt_putc(fmt_sink_t *s, char c) {
    if (s->file) {
        fwrite(&c, 1, 1, s->file);
    } else if (!s->buf) {
        putchar((int)c);
    } else if (s->pos + 1 < s->cap) { // deja lugar para el '\0' final
        s->buf[s->pos] = c;
    }
    s->pos++;
}

static inline void fmt_puts(fmt_sink_t *s, const char *str) {
    while (*str) {
        fmt_putc(s, *str++);
    }
}

static inline void fmt_uint(fmt_sink_t *s, u32 v, u32 base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[12];
    int n = 0;
    if (v == 0) {
        fmt_putc(s, '0');
        return;
    }
    while (v > 0) {
        buf[n++] = digits[v % base];
        v /= base;
    }
    while (n > 0) {
        fmt_putc(s, buf[--n]);
    }
}

static inline void fmt_int(fmt_sink_t *s, int v) {
    if (v < 0) {
        fmt_putc(s, '-');
        fmt_uint(s, (u32)(-v), 10, 0);
    } else {
        fmt_uint(s, (u32)v, 10, 0);
    }
}

static inline void vformat(fmt_sink_t *s, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            fmt_putc(s, *p);
            continue;
        }
        p++;
        switch (*p) {
            case 'd':
                fmt_int(s, va_arg(ap, int));
                break;
            case 'u':
                fmt_uint(s, va_arg(ap, unsigned int), 10, 0);
                break;
            case 'x':
                fmt_uint(s, va_arg(ap, unsigned int), 16, 0);
                break;
            case 'X':
                fmt_uint(s, va_arg(ap, unsigned int), 16, 1);
                break;
            case 'c':
                fmt_putc(s, (char)va_arg(ap, int));
                break;
            case 's':
                fmt_puts(s, va_arg(ap, const char *));
                break;
            case '%':
                fmt_putc(s, '%');
                break;
            case 0:
                p--; // "%" suelto al final de la cadena: no comerse el \0
                break;
            default:
                fmt_putc(s, '%');
                fmt_putc(s, *p);
                break;
        }
    }
}

static inline int printf(const char *fmt, ...) {
    fmt_sink_t s = {0, 0, 0, 0};
    va_list ap;
    va_start(ap, fmt);
    vformat(&s, fmt, ap);
    va_end(ap);
    return (int)s.pos;
}

static inline int vsnprintf(char *buf, u32 size, const char *fmt, va_list ap) {
    fmt_sink_t s = {buf, 0, size, 0};
    vformat(&s, fmt, ap);
    if (size) {
        buf[s.pos < size ? s.pos : size - 1] = 0; // corta seguro si se truncó
    }
    return (int)s.pos;
}

static inline int snprintf(char *buf, u32 size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

// Sin límite real de tamaño (mismo riesgo que el sprintf real — a criterio
// de quien lo use, por eso snprintf existe y es la opción recomendada).
static inline int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, 0xFFFFFFFFu, fmt, ap);
    va_end(ap);
    return r;
}

static inline int vfprintf(FILE *f, const char *fmt, va_list ap) {
    fmt_sink_t s = {0, 0, 0, f};
    vformat(&s, fmt, ap);
    return (int)s.pos;
}

static inline int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

#endif // AIZ_LIBC_STDIO_H
