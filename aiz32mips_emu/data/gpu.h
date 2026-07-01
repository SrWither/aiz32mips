// gpu.h — mini lib gráfica para AIZ-32 (freestanding, sin libc)
//
// La GPU recibe comandos como una display list de words de 32 bits en
// VRAM: el CPU arma la lista con SW normales (gpu_push) y la ejecuta de
// una con gpu_kick(). Mucho más rápido que mandar parámetro por parámetro
// por MMIO. Ver el comentario de cada registro en gpu.rs si hace falta
// el detalle exacto del formato.

#ifndef AIZ_GPU_H
#define AIZ_GPU_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;

// kseg1 (uncached, sin TLB): este código bare-metal corre antes de que
// exista ninguna entrada de TLB, así que MMIO y VRAM tienen que estar en
// un segmento sin traducción, igual que el vector de reset (0xBFC00000).
// kuseg (0x1F802000/0x10000000 "pelados") exige TLB y rompe en el primer
// acceso: por eso se suman con 0xA0000000.
#define GPU_MMIO_BASE 0xBF802000u
#define VRAM_BASE 0xB0000000u

// ── registros MMIO ──────────────────────────────────────────────────────
#define REG_FB_WIDTH (*(volatile u16 *)(GPU_MMIO_BASE + 0x00))
#define REG_FB_HEIGHT (*(volatile u16 *)(GPU_MMIO_BASE + 0x02))
#define REG_FB0_ADDR (*(volatile u32 *)(GPU_MMIO_BASE + 0x08))
#define REG_FB1_ADDR (*(volatile u32 *)(GPU_MMIO_BASE + 0x0C))
#define REG_ZBUF_ADDR (*(volatile u32 *)(GPU_MMIO_BASE + 0x10))
#define REG_STATUS (*(volatile u32 *)(GPU_MMIO_BASE + 0x14))
#define REG_CMD_ADDR (*(volatile u32 *)(GPU_MMIO_BASE + 0x18))
#define REG_CMD_LEN (*(volatile u32 *)(GPU_MMIO_BASE + 0x1C))
#define REG_CMD_KICK (*(volatile u8 *)(GPU_MMIO_BASE + 0x20))
#define REG_FONT_ADDR (*(volatile u32 *)(GPU_MMIO_BASE + 0x24))
#define REG_FONT_W (*(volatile u8 *)(GPU_MMIO_BASE + 0x28))
#define REG_FONT_H (*(volatile u8 *)(GPU_MMIO_BASE + 0x29))
#define REG_TEXT_ADDR (*(volatile u32 *)(GPU_MMIO_BASE + 0x30))
#define REG_TEXT_COLS (*(volatile u16 *)(GPU_MMIO_BASE + 0x34))
#define REG_TEXT_ROWS (*(volatile u16 *)(GPU_MMIO_BASE + 0x36))
#define REG_TEXT_PALETTE_ADDR (*(volatile u32 *)(GPU_MMIO_BASE + 0x38))
#define REG_TEXT_ENABLE (*(volatile u8 *)(GPU_MMIO_BASE + 0x3C))

#define STATUS_VBLANK (1u << 1)
#define STATUS_DRAW_FB (1u << 2)
#define STATUS_DISPLAY_FB (1u << 3)

// ── layout default de VRAM (4MB) ────────────────────────────────────────
// La GPU no impone nada de esto, son sólo los offsets que usa esta lib.
#define VRAM_FB0 0x000000u
#define VRAM_FB1 0x040000u
#define VRAM_ZBUF 0x080000u
#define VRAM_FONT 0x0C0000u
#define VRAM_TEXT 0x0C1000u
#define VRAM_PALETTE 0x0C2000u
#define VRAM_CMDBUF 0x0C3000u
#define VRAM_HEAP 0x0D3000u // libre para sprites/texturas/strings (lib o usuario)

// ── opcodes de la display list (deben matchear aiz32mips_core/devices/gpu.rs) ──
#define OP_NOP 0u
#define OP_CLEAR 1u
#define OP_CLEAR_Z 2u
#define OP_SET_CLIP 3u
#define OP_SET_BLEND 4u
#define OP_FLIP 5u
#define OP_FILLRECT 6u
#define OP_RECT_OUTLINE 7u
#define OP_LINE 8u
#define OP_CIRCLE 9u
#define OP_TRIANGLE2D 10u
#define OP_GRAD_X 11u
#define OP_GRAD_Y 12u
#define OP_GRAD_XY 13u
#define OP_PUTCHAR 14u
#define OP_PUTS 15u
#define OP_BLIT 16u
#define OP_TRIANGLE3D 17u

#define BLIT_KEY (1u << 0)
#define BLIT_ALPHA (1u << 1)
#define BLIT_FLIP_X (1u << 2)
#define BLIT_FLIP_Y (1u << 3)

#define TRI_DEPTH_TEST (1u << 0)
#define TRI_DEPTH_WRITE (1u << 1)
#define TRI_GOURAUD (1u << 2)
#define TRI_TEXTURED (1u << 3)
#define TRI_BLEND (1u << 4)

#define GPU_BLEND_OPAQUE 0u
#define GPU_BLEND_ALPHA 1u
#define GPU_BLEND_ADD 2u

// helper para armar colores ARGB8888 (alpha en el byte alto)
#define ARGB(a, r, g, b) ((u32)(((a) << 24) | ((r) << 16) | ((g) << 8) | (b)))
#define RGB(r, g, b) ARGB(0xFF, r, g, b)

// ─────────────────────────────────────────────────────────────────────────
// Builder de display list: arma comandos en VRAM_CMDBUF y los manda con
// gpu_kick(). Un solo frame puede acumular cientos de draw calls sin tocar
// el bus MMIO más que al final.
// ─────────────────────────────────────────────────────────────────────────

// volatile: si no, a -O2 el compilador no tiene forma de saber que la GPU
// va a leer esta memoria después y puede tirar las escrituras como dead
// stores (nada dentro de esta TU las vuelve a leer). Con esto rotas, el
// command buffer queda vacío y ningún ejemplo dibuja nada.
static volatile u32 *gpu_cmd_cursor;
static u32 gpu_cmd_start_words;

static inline void gpu_begin(void) {
    gpu_cmd_cursor = (volatile u32 *)(VRAM_BASE + VRAM_CMDBUF);
    gpu_cmd_start_words = 0;
}

static inline void gpu_push(u32 word) {
    *gpu_cmd_cursor = word;
    gpu_cmd_cursor++;
    gpu_cmd_start_words++;
}

static inline void gpu_end(void) {
    if (gpu_cmd_start_words == 0) return;
    REG_CMD_ADDR = VRAM_CMDBUF;
    REG_CMD_LEN = gpu_cmd_start_words;
    REG_CMD_KICK = 1;
}

// ── primitivas: cada una agrega un comando a la lista en curso ─────────

static inline void gpu_clear(u32 color) {
    gpu_push(OP_CLEAR);
    gpu_push(color);
}

static inline void gpu_clear_z(u32 value) {
    gpu_push(OP_CLEAR_Z);
    gpu_push(value);
}

static inline void gpu_set_clip(i32 x0, i32 y0, i32 x1, i32 y1) {
    gpu_push(OP_SET_CLIP);
    gpu_push((u32)x0);
    gpu_push((u32)y0);
    gpu_push((u32)x1);
    gpu_push((u32)y1);
}

static inline void gpu_set_blend(u32 mode) {
    gpu_push(OP_SET_BLEND);
    gpu_push(mode);
}

static inline void gpu_flip(void) {
    gpu_push(OP_FLIP);
}

static inline void gpu_fillrect(i32 x, i32 y, i32 w, i32 h, u32 color) {
    gpu_push(OP_FILLRECT);
    gpu_push((u32)x);
    gpu_push((u32)y);
    gpu_push((u32)w);
    gpu_push((u32)h);
    gpu_push(color);
}

static inline void gpu_rect_outline(i32 x, i32 y, i32 w, i32 h, u32 color) {
    gpu_push(OP_RECT_OUTLINE);
    gpu_push((u32)x);
    gpu_push((u32)y);
    gpu_push((u32)w);
    gpu_push((u32)h);
    gpu_push(color);
}

static inline void gpu_line(i32 x0, i32 y0, i32 x1, i32 y1, u32 color) {
    gpu_push(OP_LINE);
    gpu_push((u32)x0);
    gpu_push((u32)y0);
    gpu_push((u32)x1);
    gpu_push((u32)y1);
    gpu_push(color);
}

static inline void gpu_circle(i32 cx, i32 cy, i32 r, u32 color, int filled) {
    gpu_push(OP_CIRCLE);
    gpu_push((u32)cx);
    gpu_push((u32)cy);
    gpu_push((u32)r);
    gpu_push(color);
    gpu_push(filled ? 1u : 0u);
}

static inline void gpu_triangle2d(i32 x0, i32 y0, i32 x1, i32 y1, i32 x2, i32 y2, u32 color, int filled) {
    gpu_push(OP_TRIANGLE2D);
    gpu_push((u32)x0);
    gpu_push((u32)y0);
    gpu_push((u32)x1);
    gpu_push((u32)y1);
    gpu_push((u32)x2);
    gpu_push((u32)y2);
    gpu_push(color);
    gpu_push(filled ? 1u : 0u);
}

static inline void gpu_grad_x(u32 left, u32 right) {
    gpu_push(OP_GRAD_X);
    gpu_push(left);
    gpu_push(right);
}

static inline void gpu_grad_y(u32 top, u32 bottom) {
    gpu_push(OP_GRAD_Y);
    gpu_push(top);
    gpu_push(bottom);
}

static inline void gpu_grad_xy(u32 c00, u32 c10, u32 c01, u32 c11) {
    gpu_push(OP_GRAD_XY);
    gpu_push(c00);
    gpu_push(c10);
    gpu_push(c01);
    gpu_push(c11);
}

static inline void gpu_putchar(i32 x, i32 y, u16 ch, u32 fg, u32 bg) {
    gpu_push(OP_PUTCHAR);
    gpu_push((u32)x);
    gpu_push((u32)y);
    gpu_push(ch);
    gpu_push(fg);
    gpu_push(bg);
}

static inline u32 gpu_strlen(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

// Copia el string a un staging buffer en VRAM (arriba del heap, se va
// reusando) y referencia esa dirección desde el comando PUTS.
static inline void gpu_puts(i32 x, i32 y, u32 fg, u32 bg, const char *s) {
    static u32 staging_off = VRAM_HEAP;
    u32 len = gpu_strlen(s);
    volatile u8 *dst = (volatile u8 *)(VRAM_BASE + staging_off);
    for (u32 i = 0; i < len; i++) dst[i] = (u8)s[i];

    gpu_push(OP_PUTS);
    gpu_push((u32)x);
    gpu_push((u32)y);
    gpu_push(fg);
    gpu_push(bg);
    gpu_push(staging_off);
    gpu_push(len);
}

// src_addr/dst en offsets de VRAM (no punteros CPU con el +VRAM_BASE).
static inline void gpu_blit(u32 src_addr, u16 src_w, u16 src_h, i32 dst_x, i32 dst_y, u32 flags, u32 key) {
    gpu_push(OP_BLIT);
    gpu_push(src_addr);
    gpu_push(src_w);
    gpu_push(src_h);
    gpu_push((u32)dst_x);
    gpu_push((u32)dst_y);
    gpu_push(flags);
    gpu_push(key);
}

typedef struct {
    i32 x, y;
    u32 z;     // 0 = más cerca, 0xFFFFFFFF = más lejos
    u32 color; // ARGB8888
    u32 u, v;  // coords de textura en 0..65535 (0..1 en 16.16)
} gpu_vertex_t;

static inline void gpu_triangle3d(u32 flags, u32 tex_addr, u16 tex_w, u16 tex_h, gpu_vertex_t v0, gpu_vertex_t v1, gpu_vertex_t v2) {
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

// ── inicialización / vsync ──────────────────────────────────────────────

static inline void gpu_init(u16 w, u16 h, int double_buffer, int with_zbuffer) {
    REG_FB_WIDTH = w;
    REG_FB_HEIGHT = h;
    REG_FB0_ADDR = VRAM_FB0;
    REG_FB1_ADDR = double_buffer ? VRAM_FB1 : 0;
    REG_ZBUF_ADDR = with_zbuffer ? VRAM_ZBUF : 0;
    REG_FONT_ADDR = VRAM_FONT;
    REG_FONT_W = 8;
    REG_FONT_H = 8;
}

static inline void gpu_wait_vblank(void) {
    while (!(REG_STATUS & STATUS_VBLANK)) {
    }
}

// ── modo texto (TTY) ────────────────────────────────────────────────────
// Cada celda son 2 bytes: char (ASCII) + attr (bg<<4 | fg, paleta de 16
// colores). Se escriben directo, sin pasar por comandos: la GPU las
// compone sola sobre el framebuffer en cada OP_FLIP.

static inline void gpu_text_init(u16 cols, u16 rows) {
    REG_TEXT_ADDR = VRAM_TEXT;
    REG_TEXT_COLS = cols;
    REG_TEXT_ROWS = rows;
    REG_TEXT_PALETTE_ADDR = 0; // paleta default de 16 colores incorporada
    REG_TEXT_ENABLE = 1;
}

static inline void gpu_text_putc(u16 col, u16 row, char c, u8 fg, u8 bg) {
    volatile u8 *cell = (volatile u8 *)(VRAM_BASE + VRAM_TEXT + (row * REG_TEXT_COLS + col) * 2);
    cell[0] = (u8)c;
    cell[1] = (u8)((bg << 4) | (fg & 0x0F));
}

static inline void gpu_text_puts(u16 col, u16 row, const char *s, u8 fg, u8 bg) {
    u16 c = col;
    while (*s) {
        if (*s == '\n') {
            row++;
            c = col;
        } else {
            gpu_text_putc(c, row, *s, fg, bg);
            c++;
        }
        s++;
    }
}

static inline void gpu_text_clear(void) {
    u32 cells = (u32)REG_TEXT_COLS * REG_TEXT_ROWS;
    volatile u16 *buf = (volatile u16 *)(VRAM_BASE + VRAM_TEXT);
    for (u32 i = 0; i < cells; i++) buf[i] = 0; // char=0, attr=0 (negro sobre negro)
}

// ─────────────────────────────────────────────────────────────────────────
// Matemática 3D en punto fijo Q16.16 (no flotantes: el COP1 todavía no
// tiene aritmética real implementada, así que evitamos depender de eso).
// Suficiente para mover/rotar/proyectar vértices antes de mandarlos a
// gpu_triangle3d.
// ─────────────────────────────────────────────────────────────────────────

typedef i32 fx_t; // Q16.16
#define FX_ONE (1 << 16)
#define FX_SHIFT 16

static inline fx_t fx_from_int(i32 i) { return i << FX_SHIFT; }
static inline i32 fx_to_int(fx_t f) { return f >> FX_SHIFT; }
static inline fx_t fx_mul(fx_t a, fx_t b) { return (fx_t)(((long long)a * (long long)b) >> FX_SHIFT); }

// División 64/32 manual (shift-subtract): con -nostdlib no tenemos
// compiler-rt, así que el operador `/` de 64 bits (__divdi3/__udivdi3)
// no está disponible. Multiplicación y shifts de 64 bits sí los inlinea
// el compilador directo, división no.
static inline unsigned long long fx_udiv64(unsigned long long num, unsigned long long den) {
    if (den == 0) return 0xFFFFFFFFFFFFFFFFULL;
    unsigned long long quotient = 0;
    unsigned long long rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((num >> i) & 1ULL);
        if (rem >= den) {
            rem -= den;
            quotient |= (1ULL << i);
        }
    }
    return quotient;
}

static inline fx_t fx_div(fx_t a, fx_t b) {
    int neg = (a < 0) != (b < 0);
    unsigned long long ua = (unsigned long long)(a < 0 ? -a : a) << FX_SHIFT;
    unsigned long long ub = (unsigned long long)(b < 0 ? -b : b);
    fx_t r = (fx_t)fx_udiv64(ua, ub);
    return neg ? -r : r;
}

// seno/coseno por tabla (8.8 grados -> Q16.16), evita Taylor con floats.
// 256 entradas cubriendo 0..2*pi.
static const short fx_sin_table[256] = {
    0, 25, 50, 75, 100, 125, 150, 175, 200, 224, 249, 273, 297, 321, 345, 369,
    392, 415, 438, 460, 483, 505, 526, 548, 569, 590, 610, 630, 650, 669, 688, 706,
    724, 742, 759, 775, 792, 807, 822, 837, 851, 865, 878, 891, 903, 915, 926, 936,
    946, 955, 964, 972, 980, 987, 993, 999, 1004, 1009, 1013, 1016, 1019, 1021, 1023, 1023,
    1024, 1023, 1023, 1021, 1019, 1016, 1013, 1009, 1004, 999, 993, 987, 980, 972, 964, 955,
    946, 936, 926, 915, 903, 891, 878, 865, 851, 837, 822, 807, 792, 775, 759, 742,
    724, 706, 688, 669, 650, 630, 610, 590, 569, 548, 526, 505, 483, 460, 438, 415,
    392, 369, 345, 321, 297, 273, 249, 224, 200, 175, 150, 125, 100, 75, 50, 25,
    0, -25, -50, -75, -100, -125, -150, -175, -200, -224, -249, -273, -297, -321, -345, -369,
    -392, -415, -438, -460, -483, -505, -526, -548, -569, -590, -610, -630, -650, -669, -688, -706,
    -724, -742, -759, -775, -792, -807, -822, -837, -851, -865, -878, -891, -903, -915, -926, -936,
    -946, -955, -964, -972, -980, -987, -993, -999, -1004, -1009, -1013, -1016, -1019, -1021, -1023, -1023,
    -1024, -1023, -1023, -1021, -1019, -1016, -1013, -1009, -1004, -999, -993, -987, -980, -972, -964, -955,
    -946, -936, -926, -915, -903, -891, -878, -865, -851, -837, -822, -807, -792, -775, -759, -742,
    -724, -706, -688, -669, -650, -630, -610, -590, -569, -548, -526, -505, -483, -460, -438, -415,
    -392, -369, -345, -321, -297, -273, -249, -224, -200, -175, -150, -125, -100, -75, -50, -25,
};

// angle: 0..255 representa 0..2*pi (un "byte de ángulo", como en muchos
// motores retro). Devuelve Q16.16 con escala 1.0 = 1024 de la tabla.
static inline fx_t fx_sin(u8 angle) { return (fx_t)fx_sin_table[angle] << (FX_SHIFT - 10); }
static inline fx_t fx_cos(u8 angle) { return (fx_t)fx_sin_table[(u8)(angle + 64)] << (FX_SHIFT - 10); }

typedef struct {
    fx_t x, y, z;
} vec3_t;

typedef struct {
    fx_t m[16]; // row-major
} mat4_t;

static inline mat4_t mat4_identity(void) {
    mat4_t r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = FX_ONE;
    return r;
}

static inline mat4_t mat4_mul(mat4_t a, mat4_t b) {
    mat4_t r = {{0}};
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            long long acc = 0;
            for (int k = 0; k < 4; k++) {
                acc += (long long)a.m[row * 4 + k] * (long long)b.m[k * 4 + col];
            }
            r.m[row * 4 + col] = (fx_t)(acc >> FX_SHIFT);
        }
    }
    return r;
}

static inline mat4_t mat4_translate(fx_t x, fx_t y, fx_t z) {
    mat4_t r = mat4_identity();
    r.m[3] = x;
    r.m[7] = y;
    r.m[11] = z;
    return r;
}

static inline mat4_t mat4_rotate_y(u8 angle) {
    mat4_t r = mat4_identity();
    fx_t s = fx_sin(angle), c = fx_cos(angle);
    r.m[0] = c;
    r.m[2] = s;
    r.m[8] = -s;
    r.m[10] = c;
    return r;
}

static inline mat4_t mat4_rotate_x(u8 angle) {
    mat4_t r = mat4_identity();
    fx_t s = fx_sin(angle), c = fx_cos(angle);
    r.m[5] = c;
    r.m[6] = -s;
    r.m[9] = s;
    r.m[10] = c;
    return r;
}

static inline mat4_t mat4_rotate_z(u8 angle) {
    mat4_t r = mat4_identity();
    fx_t s = fx_sin(angle), c = fx_cos(angle);
    r.m[0] = c;
    r.m[1] = -s;
    r.m[4] = s;
    r.m[5] = c;
    return r;
}

// Transforma `v` por `m` y deja el resultado en espacio "vista" (sin
// dividir por w todavía, eso lo hace gpu_project_vertex).
static inline vec3_t mat4_transform(mat4_t m, vec3_t v) {
    vec3_t r;
    r.x = fx_mul(m.m[0], v.x) + fx_mul(m.m[1], v.y) + fx_mul(m.m[2], v.z) + m.m[3];
    r.y = fx_mul(m.m[4], v.x) + fx_mul(m.m[5], v.y) + fx_mul(m.m[6], v.z) + m.m[7];
    r.z = fx_mul(m.m[8], v.x) + fx_mul(m.m[9], v.y) + fx_mul(m.m[10], v.z) + m.m[11];
    return r;
}

// Proyección perspectiva simple (sin matriz 4x4 homogénea: alcanza con
// dividir x,y por z para algo "primitivo" estilo PS1) + viewport a
// píxeles de pantalla. `focal` típicamente ~= ancho de pantalla.
static inline gpu_vertex_t gpu_project_vertex(vec3_t cam_space, fx_t focal, u32 color, u16 screen_w, u16 screen_h) {
    gpu_vertex_t out;
    fx_t z = cam_space.z;
    if (z < FX_ONE / 16) z = FX_ONE / 16; // evita división por ~0 / detrás de cámara

    fx_t px = fx_mul(fx_div(cam_space.x, z), focal);
    fx_t py = fx_mul(fx_div(cam_space.y, z), focal);

    out.x = fx_to_int(px) + screen_w / 2;
    out.y = screen_h / 2 - fx_to_int(py);
    // mapeo de Z a 0..65535 para el Z-buffer (u16): asumimos cam_space.z
    // ya viene en un rango razonable (ej. 1..64 unidades de mundo).
    i32 zi = fx_to_int(z) * 1000;
    out.z = (zi < 0) ? 0 : (zi > 65535 ? 65535 : (u32)zi);
    out.color = color;
    out.u = 0;
    out.v = 0;
    return out;
}

#endif // AIZ_GPU_H
