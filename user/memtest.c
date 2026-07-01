// memtest.c — prueba el heap con demanda de páginas (ver kernel/mm.c::
// mm_handle_page_fault): aloca bastante más de lo que entraba en el heap
// fijo viejo de 8KB, toca una página de cada 4KB (para forzar que cada
// una dispare su propio TLB refill) y relee para confirmar que el dato
// sigue siendo el que se esperaba.
#include "libc/stdio.h"
#include "libc/stdlib.h"

#define CHUNK 65536u
#define NCHUNKS 16u // 16 * 64KB = 1MB, bien por encima de las 8KB del heap fijo viejo

void _start(void) {
    u32 ok = 1;
    for (u32 c = 0; c < NCHUNKS; c++) {
        u8 *buf = (u8 *)malloc(CHUNK);
        if (!buf) {
            printf("malloc fallo en chunk %u\n", c);
            ok = 0;
            break;
        }
        for (u32 i = 0; i < CHUNK; i += 4096) {
            buf[i] = (u8)(c + 1);
        }
        for (u32 i = 0; i < CHUNK; i += 4096) {
            if (buf[i] != (u8)(c + 1)) {
                printf("dato corrupto en chunk %u offset %u\n", c, i);
                ok = 0;
            }
        }
    }
    if (ok) {
        printf("heap con demanda de paginas: 1MB alocado y verificado ok\n");
    }
    exit(0);
}
