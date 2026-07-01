// malloc.h — allocator chico para userland: free-list simple (first-fit,
// fusiona con el siguiente bloque libre al hacer free), sin libc de por
// medio. Opera sobre USER_HEAP_VADDR/USER_HEAP_SIZE (kernel/kernel.h): dos
// páginas fijas que el kernel ya deja mapeadas al spawnear el proceso
// (ver kernel/sched.c y kernel/mm.c::mm_map_user) — no hay brk/sbrk
// todavía, el heap no crece más allá de esas 8KB.
#ifndef AIZ_MALLOC_H
#define AIZ_MALLOC_H

typedef unsigned int u32;

#define HEAP_BASE 0x00401000u
#define HEAP_SIZE 0x2000u

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
    size = (size + 7u) & ~7u; // alineado a 8: los gpu_vertex_t/fx_t lo agradecen

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
    return 0; // sin memoria (heap de 8KB, no hay a dónde crecer)
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

#endif // AIZ_MALLOC_H
