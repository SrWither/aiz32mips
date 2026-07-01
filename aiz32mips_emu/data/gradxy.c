// gradxy.c — gradiente bilineal (4 colores de esquina), vía display list.
#include "gpu.h"

void _start() {
    gpu_init(320, 200, 0, 0);

    gpu_begin();
    gpu_grad_xy(0xFF3030C0, 0xFFC050C0, 0xFF30C080, 0xFFF5D060);
    gpu_end();

    while (1) {
        gpu_wait_vblank();
    }
}
