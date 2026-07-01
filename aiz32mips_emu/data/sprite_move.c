// sprite_move.c — mueve un sprite con las flechas del teclado.
// A diferencia de typewriter.c (que usa eventos de texto ya traducidos),
// acá usamos eventos de tecla cruda (pressed/released) para poder saber si
// una flecha está *sostenida*, no sólo si se apretó una vez.
#include "gpu.h"
#include "keyboard.h"

#define W 320
#define H 200
#define SPR 16
#define SPR_ADDR VRAM_HEAP
#define KEY 0xFFFF00FFu // magenta = "transparente"
#define SPEED 3

static void build_sprite(void) {
    volatile u32 *px = (volatile u32 *)(VRAM_BASE + SPR_ADDR);
    for (int y = 0; y < SPR; y++) {
        for (int x = 0; x < SPR; x++) {
            int dx = x - SPR / 2, dy = y - SPR / 2;
            int r2 = dx * dx + dy * dy;
            u32 c;
            if (r2 > (SPR / 2) * (SPR / 2)) {
                c = KEY;
            } else if (r2 > (SPR / 2 - 2) * (SPR / 2 - 2)) {
                c = RGB(20, 20, 20);
            } else {
                c = RGB(80, 180, 255);
            }
            px[y * SPR + x] = c;
        }
    }
}

static int held_up = 0, held_down = 0, held_left = 0, held_right = 0;

static void process_input(void) {
    while (kbd_has_event()) {
        u32 ev = kbd_read_event();
        if (ev & KBD_EVT_KIND_TEXT) {
            continue; // este ejemplo sólo mira teclas crudas
        }
        u8 keycode = (u8)(ev & 0xFF);
        int pressed = (ev & KBD_EVT_PRESSED) != 0;
        if (keycode == KBD_KC_UP) held_up = pressed;
        else if (keycode == KBD_KC_DOWN) held_down = pressed;
        else if (keycode == KBD_KC_LEFT) held_left = pressed;
        else if (keycode == KBD_KC_RIGHT) held_right = pressed;
    }
}

void _start() {
    gpu_init(W, H, 1, 0);
    gpu_text_init(40, 25);
    gpu_text_puts(0, 0, "flechas para mover", 11, 0);
    build_sprite();

    i32 x = (W - SPR) / 2;
    i32 y = (H - SPR) / 2;

    while (1) {
        process_input();

        if (held_up) y -= SPEED;
        if (held_down) y += SPEED;
        if (held_left) x -= SPEED;
        if (held_right) x += SPEED;

        if (x < 0) x = 0;
        if (x > W - SPR) x = W - SPR;
        if (y < 0) y = 0;
        if (y > H - SPR) y = H - SPR;

        gpu_begin();
        gpu_clear(RGB(10, 10, 30));
        gpu_blit(SPR_ADDR, SPR, SPR, x, y, BLIT_KEY, KEY);
        gpu_flip();
        gpu_end();

        gpu_wait_vblank();
    }
}
