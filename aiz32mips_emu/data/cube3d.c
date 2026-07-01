// cube3d.c — cubo girando, sombreado plano por cara, con Z-buffer.
// Muestra el rasterizador 3D "primitivo" (triángulos ya proyectados por
// software, estilo PS1): acá la proyección/rotación las hace esta lib en
// punto fijo, la GPU sólo rellena con test de profundidad.
#include "gpu.h"

#define W 320
#define H 200

static const vec3_t cube_verts[8] = {
    {-65536, -65536, -65536}, {65536, -65536, -65536}, {65536, 65536, -65536}, {-65536, 65536, -65536},
    {-65536, -65536, 65536}, {65536, -65536, 65536}, {65536, 65536, 65536}, {-65536, 65536, 65536},
};

// cada cara: 4 índices (quad) + color
static const u8 faces[6][4] = {
    {0, 1, 2, 3}, // atrás
    {5, 4, 7, 6}, // adelante
    {4, 0, 3, 7}, // izquierda
    {1, 5, 6, 2}, // derecha
    {4, 5, 1, 0}, // abajo
    {3, 2, 6, 7}, // arriba
};
static const u32 face_colors[6] = {
    RGB(220, 40, 40), RGB(40, 220, 40), RGB(40, 80, 220),
    RGB(220, 200, 40), RGB(200, 40, 200), RGB(40, 200, 200),
};

void _start() {
    gpu_init(W, H, 1, 1); // double buffer + Z-buffer

    u8 ax = 0, ay = 0;

    while (1) {
        mat4_t r = mat4_mul(mat4_rotate_y(ay), mat4_rotate_x(ax));
        mat4_t mv = mat4_mul(mat4_translate(0, 0, fx_from_int(6)), r);

        gpu_vertex_t proj[8];
        for (int i = 0; i < 8; i++) {
            vec3_t cam = mat4_transform(mv, cube_verts[i]);
            proj[i] = gpu_project_vertex(cam, fx_from_int(W / 2), 0, W, H);
        }

        gpu_begin();
        gpu_clear(RGB(5, 5, 15));
        gpu_clear_z(0xFFFF);

        u32 flags = TRI_DEPTH_TEST | TRI_DEPTH_WRITE;
        for (int f = 0; f < 6; f++) {
            gpu_vertex_t v0 = proj[faces[f][0]];
            gpu_vertex_t v1 = proj[faces[f][1]];
            gpu_vertex_t v2 = proj[faces[f][2]];
            gpu_vertex_t v3 = proj[faces[f][3]];
            v0.color = v1.color = v2.color = v3.color = face_colors[f];
            gpu_triangle3d(flags, 0, 0, 0, v0, v1, v2);
            gpu_triangle3d(flags, 0, 0, 0, v0, v2, v3);
        }

        gpu_flip();
        gpu_end();

        gpu_wait_vblank();
        ax += 1;
        ay += 2;
    }
}
