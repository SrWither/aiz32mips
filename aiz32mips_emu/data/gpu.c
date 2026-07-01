// gpu.c — el CPU escribe directo sobre el framebuffer (sin pasar por la
// GPU) para hacer un efecto de plasma. La VRAM es memoria normal mapeada
// en el bus: cualquier SW a VRAM_BASE+offset pinta un píxel, convive sin
// problema con los comandos de la GPU (que sólo tocan VRAM cuando vos le
// mandás un comando).
#include "gpu.h"

#define W 320
#define H 200

static inline u8 wave(u8 a) {
    return (u8)((fx_sin_table[a] * 127) / 1024 + 128);
}

void _start() {
    gpu_init(W, H, 1, 0); // double buffer para no pintar sobre lo que se está mostrando

    volatile u32 *fb;
    u8 t = 0;

    while (1) {
        // dibujamos siempre sobre el back buffer (el que NO se está
        // mostrando ahora mismo, según STATUS.DRAW_FB)
        u32 draw_fb_addr = (REG_STATUS & STATUS_DRAW_FB) ? VRAM_FB1 : VRAM_FB0;
        fb = (volatile u32 *)(VRAM_BASE + draw_fb_addr);

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                u8 idx = (u8)(x + y + t);
                u8 idx2 = (u8)(x * 2 - y + t * 3);
                u8 r = wave(idx);
                u8 g = wave((u8)(idx2 + 85));
                u8 b = wave((u8)(idx + idx2 + 170));
                fb[y * W + x] = RGB(r, g, b);
            }
        }

        // un solo comando (FLIP) para mostrar lo que acabamos de pintar a mano
        gpu_begin();
        gpu_flip();
        gpu_end();

        gpu_wait_vblank();
        t += 2;
    }
}
