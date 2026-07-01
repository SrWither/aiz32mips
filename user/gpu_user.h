// gpu_user.h — mismo API de comandos que aiz32mips_emu/data/gpu.h, pero
// para userland: kuseg no tiene acceso a VRAM/MMIO (viven en kseg1, fuera
// del alcance de cualquier TLB), así que acá se arma la display list en un
// buffer local del propio proceso y se manda entera de una con una sola
// syscall (sys_gpu_submit) — el kernel la copia a VRAM_CMDBUF y hace el
// kick (ver kernel/trap.c). Mismos nombres de función que gpu.h a
// propósito: "portar" un demo es -en teoría- nada más que cambiar el
// #include.
#ifndef AIZ_GPU_USER_H
#define AIZ_GPU_USER_H

#include "../kernel/abi.h"
#include "../aiz32mips_emu/data/gpu_math.h"
#include "malloc.h"

// Mismos opcodes/flags que gpu.h (tienen que matchear
// aiz32mips_core::devices::gpu) — subset chico, sólo lo que usa cube3d.c
// por ahora. Se puede ir sumando a medida que haga falta.
#define OP_CLEAR 1u
#define OP_CLEAR_Z 2u
#define OP_FLIP 5u
#define OP_TRIANGLE3D 17u

#define TRI_DEPTH_TEST (1u << 0)
#define TRI_DEPTH_WRITE (1u << 1)
#define TRI_GOURAUD (1u << 2)
#define TRI_TEXTURED (1u << 3)
#define TRI_BLEND (1u << 4)

#define GPU_STATUS_VBLANK (1u << 1)

// El buffer de comandos vive en el heap (malloc.h), no en .bss: código +
// datos + pila de llamadas ya vienen ajustados a la única página de texto
// del proceso (ver kernel/mm.c), y una escena con unos cuantos triángulos
// no entra ahí (cube3d.c manda 269 words por frame — 2 clear + 2 clear_z +
// 12 triángulos*22 + 1 flip). 320 words (1280 bytes) da margen sin
// acercarse a las 8KB del heap.
#define GPU_CMD_MAX_WORDS 320

static u32 *gpu_cmd_buf;
static u32 gpu_cmd_len;

static inline void gpu_begin(void) {
    if (!gpu_cmd_buf) {
        gpu_cmd_buf = (u32 *)malloc(GPU_CMD_MAX_WORDS * sizeof(u32));
    }
    gpu_cmd_len = 0;
}

static inline void gpu_push(u32 word) {
    if (gpu_cmd_buf && gpu_cmd_len < GPU_CMD_MAX_WORDS) {
        gpu_cmd_buf[gpu_cmd_len++] = word;
    }
}

static inline void gpu_end(void) {
    if (gpu_cmd_len > 0) {
        sys_gpu_submit(gpu_cmd_buf, gpu_cmd_len);
    }
}

static inline void gpu_clear(u32 color) {
    gpu_push(OP_CLEAR);
    gpu_push(color);
}

static inline void gpu_clear_z(u32 value) {
    gpu_push(OP_CLEAR_Z);
    gpu_push(value);
}

static inline void gpu_flip(void) {
    gpu_push(OP_FLIP);
}

static inline void gpu_triangle3d(u32 flags, u32 tex_addr, u16 tex_w, u16 tex_h, gpu_vertex_t v0, gpu_vertex_t v1,
                                   gpu_vertex_t v2) {
    gpu_push(OP_TRIANGLE3D);
    gpu_push(flags);
    gpu_push(tex_addr);
    gpu_push(((u32)tex_h << 16) | tex_w);
    gpu_vertex_t verts[3] = {v0, v1, v2};
    for (int i = 0; i < 3; i++) {
        gpu_push((u32)verts[i].x);
        gpu_push((u32)verts[i].y);
        gpu_push(verts[i].z);
        gpu_push(verts[i].color);
        gpu_push(verts[i].u);
        gpu_push(verts[i].v);
    }
}

static inline void gpu_init(u16 w, u16 h, int double_buffer, int with_zbuffer) {
    sys_gpu_init(w, h, (unsigned int)double_buffer, (unsigned int)with_zbuffer);
}

static inline void gpu_wait_vblank(void) {
    while (!(sys_gpu_status() & GPU_STATUS_VBLANK)) {
    }
}

#endif // AIZ_GPU_USER_H
