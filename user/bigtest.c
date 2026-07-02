// bigtest.c — prueba que un binario cuyo texto+datos+bss supera el viejo
// limite fijo de una sola pagina (4KB, ver el comentario que tenia
// sched_spawn) carga y corre bien ahora que esa region es demand-paged
// (kernel/mm.c, mismo mecanismo que el heap: kernel/sched.c::
// sched_load_text_page re-lee cada pagina del ELF en disco recien cuando
// el proceso la toca por primera vez).
//
// 4 tablas de 2048 u32 en .rodata (8KB c/u, 32KB en total — bien por
// encima del viejo limite de 4KB para TODO el proceso) con un valor fijo
// distinto cada una: si alguna pagina no se hubiera cargado (offset de
// archivo mal calculado, limite de tamaño rechazando el spawn, etc.) la
// suma de esa franja puntual se rompe, no solo "algo anda mal".
#include "libc/stdio.h"
#include "libc/stdlib.h"

#define R2(x) x, x
#define R4(x) R2(x), R2(x)
#define R8(x) R4(x), R4(x)
#define R16(x) R8(x), R8(x)
#define R32(x) R16(x), R16(x)
#define R64(x) R32(x), R32(x)
#define R128(x) R64(x), R64(x)
#define R256(x) R128(x), R128(x)
#define R512(x) R256(x), R256(x)
#define R1024(x) R512(x), R512(x)
#define R2048(x) R1024(x), R1024(x)

static const u32 tabla_a[2048] = {R2048(0x11111111u)};
static const u32 tabla_b[2048] = {R2048(0x22222222u)};
static const u32 tabla_c[2048] = {R2048(0x33333333u)};
static const u32 tabla_d[2048] = {R2048(0x44444444u)};

static u32 sum(const u32 *t, u32 n) {
    u32 s = 0;
    for (u32 i = 0; i < n; i++) {
        s += t[i];
    }
    return s;
}

void _start(void) {
    u32 ok = 1;
    if (sum(tabla_a, 2048) != 0x11111111u * 2048u) {
        printf("tabla_a corrupta\n");
        ok = 0;
    }
    if (sum(tabla_b, 2048) != 0x22222222u * 2048u) {
        printf("tabla_b corrupta\n");
        ok = 0;
    }
    if (sum(tabla_c, 2048) != 0x33333333u * 2048u) {
        printf("tabla_c corrupta\n");
        ok = 0;
    }
    if (sum(tabla_d, 2048) != 0x44444444u * 2048u) {
        printf("tabla_d corrupta\n");
        ok = 0;
    }
    if (ok) {
        printf("binario grande (32KB+ .rodata, texto multipagina): datos ok\n");
    }
    exit(0);
}
