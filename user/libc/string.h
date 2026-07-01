// string.h — subset chico de <string.h> para userland freestanding: con
// -nostdlib no existe memcpy/memset/etc a menos que los escribamos
// nosotros. Bonus no buscado: al definirlos ACÁ con esos nombres exactos,
// si -O2 alguna vez decide bajar un copy/zero de struct grande a una
// llamada a "memcpy"/"memset" (algo que ya pasó una vez en este proyecto,
// ver el comentario de tf_copy en kernel/sched.c), hay una definición real
// a la que enlazar en vez de un símbolo externo inexistente.
#ifndef AIZ_LIBC_STRING_H
#define AIZ_LIBC_STRING_H

#ifndef AIZ_GPU_TYPES_DEFINED
#define AIZ_GPU_TYPES_DEFINED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
#endif

static inline void *memcpy(void *dst, const void *src, u32 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u32 i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

static inline void *memmove(void *dst, const void *src, u32 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d < s) {
        for (u32 i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (u32 i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

static inline void *memset(void *dst, int val, u32 n) {
    u8 *d = (u8 *)dst;
    for (u32 i = 0; i < n; i++) {
        d[i] = (u8)val;
    }
    return dst;
}

static inline int memcmp(const void *a, const void *b, u32 n) {
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    for (u32 i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

static inline u32 strlen(const char *s) {
    u32 n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static inline char *strcpy(char *dst, const char *src) {
    u32 i = 0;
    for (; src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
    return dst;
}

static inline char *strncpy(char *dst, const char *src, u32 n) {
    u32 i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

static inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

static inline int strncmp(const char *a, const char *b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0) {
            return (int)(u8)a[i] - (int)(u8)b[i];
        }
    }
    return 0;
}

static inline char *strcat(char *dst, const char *src) {
    u32 i = strlen(dst);
    u32 j = 0;
    for (; src[j]; j++) {
        dst[i + j] = src[j];
    }
    dst[i + j] = 0;
    return dst;
}

#endif // AIZ_LIBC_STRING_H
