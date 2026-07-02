// stdlib.h — malloc/free (free-list simple, first-fit, fusiona con el
// siguiente bloque libre al hacer free) sobre el heap de 16MB *virtuales*
// que el kernel resuelve con demanda de páginas (USER_HEAP_VADDR/
// USER_HEAP_SIZE en kernel/kernel.h, ver kernel/mm.c::mm_handle_page_fault):
// nada de esos 16MB es RAM real hasta que malloc realmente entrega un
// puntero ahí adentro y el proceso lo toca — recién en ese momento el
// kernel arma la página física. Sin brk/sbrk explícito: como el heap
// entero ya es "válido" de punta a punta (aunque no esté respaldado
// todavía), no hace falta pedirle permiso al kernel para crecer. + exit()
// sobre sys_exit().
#ifndef AIZ_LIBC_STDLIB_H
#define AIZ_LIBC_STDLIB_H

#ifndef AIZ_GPU_TYPES_DEFINED
#define AIZ_GPU_TYPES_DEFINED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
#endif

#include "../../kernel/abi.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define HEAP_BASE 0x10000000u
#define HEAP_SIZE 0x01000000u

typedef struct block_header {
    u32 size; // bytes de datos que sigue al header, sin contarlo a él
    int free;
    struct block_header *next;
} block_header_t;

static block_header_t *heap_free_list = 0;

static inline void heap_init_once(void) {
    if (heap_free_list) {
        return;
    }
    heap_free_list = (block_header_t *)HEAP_BASE;
    heap_free_list->size = HEAP_SIZE - sizeof(block_header_t);
    heap_free_list->free = 1;
    heap_free_list->next = 0;
}

static inline void *malloc(u32 size) {
    heap_init_once();
    size = (size + 7u) & ~7u; // alineado a 8: gpu_vertex_t/fx_t lo agradecen

    block_header_t *b = heap_free_list;
    while (b) {
        if (b->free && b->size >= size) {
            // parte el bloque si sobra espacio como para otro header +
            // algo de datos; si no, se entrega entero (evita fragmentos
            // inservibles más chicos que un header).
            if (b->size >= size + sizeof(block_header_t) + 8u) {
                block_header_t *rest = (block_header_t *)((char *)b + sizeof(block_header_t) + size);
                rest->size = b->size - size - sizeof(block_header_t);
                rest->free = 1;
                rest->next = b->next;
                b->next = rest;
                b->size = size;
            }
            b->free = 0;
            return (char *)b + sizeof(block_header_t);
        }
        b = b->next;
    }
    return 0; // sin memoria (se acabaron los 16MB virtuales del heap)
}

static inline void free(void *ptr) {
    if (!ptr) {
        return;
    }
    block_header_t *b = (block_header_t *)((char *)ptr - sizeof(block_header_t));
    b->free = 1;
    // Fusiona con el siguiente si también está libre: sin esto, alocar y
    // liberar seguido termina fragmentando el heap en bloques cada vez
    // más chicos. No fusiona hacia atrás (alcanza para este primer paso;
    // requeriría una lista doblemente enlazada o recorrer desde el inicio).
    if (b->next && b->next->free) {
        b->size += sizeof(block_header_t) + b->next->size;
        b->next = b->next->next;
    }
}

// nmemb*size sin chequeo de overflow: mismo alcance que malloc de este
// proyecto (bump/first-fit sin garantías más allá de "alcanza para lo que
// se necesita hoy").
static inline void *calloc(u32 nmemb, u32 size) {
    u32 total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        u8 *b = (u8 *)p;
        for (u32 i = 0; i < total; i++) {
            b[i] = 0;
        }
    }
    return p;
}

// Si el bloque actual ya entra, lo devuelve tal cual (no achica ni parte —
// mismo criterio "lo simple que alcanza" que malloc/free de este archivo).
// Si no entra, pide uno nuevo, copia y libera el viejo — sin intentar
// crecer in-place hacia el siguiente bloque libre.
static inline void *realloc(void *ptr, u32 size) {
    if (!ptr) {
        return malloc(size);
    }
    if (!size) {
        free(ptr);
        return 0;
    }
    block_header_t *b = (block_header_t *)((char *)ptr - sizeof(block_header_t));
    if (b->size >= size) {
        return ptr;
    }
    void *n = malloc(size);
    if (!n) {
        return 0;
    }
    u32 copy = b->size < size ? b->size : size;
    u8 *d = (u8 *)n;
    const u8 *s = (const u8 *)ptr;
    for (u32 i = 0; i < copy; i++) {
        d[i] = s[i];
    }
    free(ptr);
    return n;
}

static inline int abs(int x) {
    return x < 0 ? -x : x;
}

// atoi/atol/strtol: parsing de enteros decimales (atoi/atol) o en la base
// que se pida (strtol, con base=0 = auto-detectar "0x"/"0" como hex/octal,
// igual que el strtol real). Sin chequeo de overflow ni de errno (este
// proyecto no tiene errno todavía) — para eso está `endptr`, que sí
// respeta el contrato real (apunta al primer carácter no consumido).
static inline int atoi(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') {
        s++;
    }
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

static inline long atol(const char *s) {
    return (long)atoi(s); // int y long son los dos de 32 bits en este target
}

static inline long strtol(const char *s, char **endptr, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n') {
        s++;
    }
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
        s++;
    } else if (base == 0) {
        base = 10;
    }
    long v = 0;
    while (1) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
        s++;
    }
    if (endptr) {
        *endptr = (char *)s;
    }
    return v * sign;
}

// sys_exit() no lleva status de salida (no hay quién lo lea todavía: sin
// proceso padre esperando, ver kernel/sched.c) — se ignora, no se pierde.
static inline void exit(int code) {
    (void)code;
    sys_exit();
}

#endif // AIZ_LIBC_STDLIB_H
