// gradx.c — gradiente horizontal simple, vía display list.
#include "gpu.h"

void _start() {
    gpu_init(320, 200, 0, 0);

    gpu_begin();
    gpu_grad_x(RGB(0, 0, 255), RGB(255, 0, 0));
    gpu_end();

    while (1) {
        gpu_wait_vblank();
    }
}
