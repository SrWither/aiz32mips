// gpu.h — mini lib gráfica para AIZ-32 (freestanding, sin libc)
//
// La GPU recibe comandos como una display list de words de 32 bits en
// VRAM: el CPU arma la lista con SW normales (gpu_push) y la ejecuta de
// una con gpu_kick(). Mucho más rápido que mandar parámetro por parámetro
// por MMIO. Ver el comentario de cada registro en gpu.rs si hace falta
// el detalle exacto del formato.

#ifndef AIZ_GPU_H
#define AIZ_GPU_H

#ifndef AIZ_GPU_TYPES_DEFINED
#define AIZ_GPU_TYPES_DEFINED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
#endif

// gpu_vertex_t, fx_t/mat4_t/vec3_t y gpu_project_vertex viven en
// gpu_math.h (no tocan VRAM/MMIO, las reusa también userland — ver
// user/gpu_user.h). Se incluye acá arriba porque gpu_triangle3d ya
// necesita gpu_vertex_t.
#include "gpu_math.h"

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
// Región aparte para las display lists que llegan por syscall (sys_gpu_submit,
// ver kernel/trap.c y user/gpu_user.h): el kernel usa VRAM_CMDBUF para sus
// propios flips triviales de consola (console_flush(), que corre en cada
// tick de timer/tecla) — si un proceso de usuario escribiera ahí también,
// se pisarían entre sí (exactamente lo que pasaba antes de este define: el
// cubo desaparecía porque el próximo parpadeo del cursor pisaba su display
// list con un OP_FLIP suelto antes de que la GPU llegara a ejecutarla).
#define VRAM_USER_CMDBUF 0x0C8000u
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

// ARGB/RGB: en gpu_math.h (ya incluido arriba)

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

// gpu_vertex_t: en gpu_math.h (ya incluido arriba)

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

// char=' ' (no 0): op_flip (aiz32mips_core::devices::gpu) trata la celda
// char=0 como "nunca tocada" y se saltea el blit — optimización para no
// dibujar de más en el primer flip, pero rompe cualquier intento de borrar
// una celda que ya tenía contenido (el framebuffer sólo se limpia una vez,
// en console_init). Con ' ' sí se dibuja el glyph (negro sobre negro) y
// tapa lo que hubiera antes.
#define TEXT_BLANK_CELL 0x0020u

static inline void gpu_text_clear(void) {
    u32 cells = (u32)REG_TEXT_COLS * REG_TEXT_ROWS;
    volatile u16 *buf = (volatile u16 *)(VRAM_BASE + VRAM_TEXT);
    for (u32 i = 0; i < cells; i++) buf[i] = TEXT_BLANK_CELL;
}

// Sube todo un renglón: fila 0 se pierde, la última queda en blanco. VRAM_TEXT
// es memoria plana (sin comandos de por medio), así que es una copia directa.
static inline void gpu_text_scroll(void) {
    volatile u16 *buf = (volatile u16 *)(VRAM_BASE + VRAM_TEXT);
    u16 cols = REG_TEXT_COLS, rows = REG_TEXT_ROWS;
    for (u16 row = 1; row < rows; row++) {
        for (u16 col = 0; col < cols; col++) {
            buf[(row - 1) * cols + col] = buf[row * cols + col];
        }
    }
    for (u16 col = 0; col < cols; col++) {
        buf[(rows - 1) * cols + col] = TEXT_BLANK_CELL;
    }
}

#endif // AIZ_GPU_H
