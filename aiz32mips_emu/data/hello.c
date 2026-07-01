// hello.c — demo de la consola por hardware (modo texto).
// Mucho más simple que dibujar el string con PUTS: las celdas se escriben
// directo, la GPU las compone sola en cada flip.
#include "gpu.h"

void _start() {
    gpu_init(320, 200, 0, 0);
    gpu_text_init(40, 25);
    gpu_text_clear();

    gpu_text_puts(14, 12, "Hello, AIZ-32!", 15, 0);
    gpu_text_puts(8, 14, "consola de texto por hardware", 8, 0);

    gpu_begin();
    gpu_clear(RGB(0, 0, 20));
    gpu_flip(); // compone el layer de texto sobre el FB
    gpu_end();

    while (1) {
        gpu_wait_vblank();
    }
}
