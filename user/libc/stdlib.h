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

static inline int abs(int x) {
    return x < 0 ? -x : x;
}

// sys_exit() no lleva status de salida (no hay quién lo lea todavía: sin
// proceso padre esperando, ver kernel/sched.c) — se ignora, no se pierde.
static inline void exit(int code) {
    (void)code;
    sys_exit();
}

#endif // AIZ_LIBC_STDLIB_H
