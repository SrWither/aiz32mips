// gradxy_anim.c — gradiente animado con double buffer + vsync.
//
// La versión vieja de este demo usaba `float` (sin_taylor a mano), pero el
// COP1 de este core todavía no tiene aritmética real implementada (sólo
// mov/load/store), así que esas cuentas terminaban siendo no-ops. Acá se
// usa la tabla de seno en punto fijo de gpu.h, que sí corre en el CPU
// entero sin depender del FPU.
#include "gpu.h"

static inline u8 f2c(fx_t s /* -1024..1024 aprox, Q16.16 escala 1.0=1024 */) {
    // s viene de fx_sin/fx_cos: rango aprox [-1024,1024] en unidades crudas
    // de la tabla (no Q16.16 todavía acá, lo tratamos como entero chico).
    i32 v = (s * 255) / 2048 + 128;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (u8)v;
}

void _start() {
    gpu_init(320, 200, 1, 0); // double buffer, sin Z (esto es 2D)

    u8 t = 0;
    while (1) {
        u8 a0 = t, a1 = (u8)(t + 21), a2 = (u8)(t + 43), a3 = (u8)(t + 64);

        u32 c00 = ARGB(0xFF, f2c(fx_sin_table[a0]), f2c(fx_sin_table[(u8)(a0 + 64)]), f2c(fx_sin_table[(u8)(a0 + 128)]));
        u32 c10 = ARGB(0xFF, f2c(fx_sin_table[a1]), f2c(fx_sin_table[(u8)(a1 + 64)]), f2c(fx_sin_table[(u8)(a1 + 128)]));
        u32 c01 = ARGB(0xFF, f2c(fx_sin_table[a2]), f2c(fx_sin_table[(u8)(a2 + 64)]), f2c(fx_sin_table[(u8)(a2 + 128)]));
        u32 c11 = ARGB(0xFF, f2c(fx_sin_table[a3]), f2c(fx_sin_table[(u8)(a3 + 64)]), f2c(fx_sin_table[(u8)(a3 + 128)]));

        gpu_begin();
        gpu_grad_xy(c00, c10, c01, c11);
        gpu_flip(); // pasa lo recién dibujado a front, sin tearing
        gpu_end();

        gpu_wait_vblank();
        t += 1;
    }
}
