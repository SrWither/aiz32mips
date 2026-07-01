// sprite.c — sprite con transparencia por color-key, rebotando en pantalla.
// El sprite se genera a mano en VRAM (no hay pipeline de assets todavía),
// pero gpu_blit funciona igual con cualquier bloque ARGB8888 en VRAM.
#include "gpu.h"

#define W 320
#define H 200
#define SPR 16
#define SPR_ADDR VRAM_HEAP
#define KEY 0xFFFF00FFu // magenta = "transparente"

static void build_sprite(void) {
    volatile u32 *px = (volatile u32 *)(VRAM_BASE + SPR_ADDR);
    for (int y = 0; y < SPR; y++) {
        for (int x = 0; x < SPR; x++) {
            int dx = x - SPR / 2, dy = y - SPR / 2;
            int r2 = dx * dx + dy * dy;
            u32 c;
            if (r2 > (SPR / 2) * (SPR / 2)) {
                c = KEY; // fuera del círculo: transparente
            } else if (r2 > (SPR / 2 - 2) * (SPR / 2 - 2)) {
                c = RGB(20, 20, 20); // borde
            } else {
                c = RGB(255, 200, 0); // relleno
            }
            px[y * SPR + x] = c;
        }
    }
}

void _start() {
    gpu_init(W, H, 1, 0);
    build_sprite();

    i32 x = 10, y = 10, vx = 2, vy = 1;

    while (1) {
        x += vx;
        y += vy;
        if (x < 0 || x > W - SPR) vx = -vx;
        if (y < 0 || y > H - SPR) vy = -vy;

        gpu_begin();
        gpu_clear(RGB(10, 10, 30));
        gpu_blit(SPR_ADDR, SPR, SPR, x, y, BLIT_KEY, KEY);
        gpu_flip();
        gpu_end();

        gpu_wait_vblank();
    }
}
