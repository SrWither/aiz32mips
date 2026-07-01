// typewriter.c — consola simple: hace eco de lo que escribís usando el
// modo texto por hardware + el FIFO de teclado (eventos ya traducidos a
// ASCII por el host, con shift/layout aplicado).
#include "gpu.h"
#include "keyboard.h"

#define COLS 40
#define ROWS 25

static int cursor_col = 0;
static int cursor_row = 2;

static void newline(void) {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= ROWS) {
        // sin scroll real todavía: al llegar abajo, reinicia arriba
        cursor_row = 2;
        gpu_text_clear();
        gpu_text_puts(0, 0, "escribi algo (echo de teclado):", 11, 0);
    }
}

static void put_char(char c) {
    if (c == '\n' || c == '\r') {
        newline();
        return;
    }
    if (c == 8 || c == 127) { // backspace / delete
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 2) {
            cursor_row--;
            cursor_col = COLS - 1;
        }
        gpu_text_putc(cursor_col, cursor_row, ' ', 15, 0);
        return;
    }
    if (c < 32 || c > 126) {
        return; // no imprimible, lo ignoramos
    }
    gpu_text_putc(cursor_col, cursor_row, c, 15, 0);
    cursor_col++;
    if (cursor_col >= COLS) {
        newline();
    }
}

void _start() {
    gpu_init(320, 200, 0, 0);
    gpu_text_init(COLS, ROWS);
    gpu_text_clear();
    gpu_text_puts(0, 0, "escribi algo (echo de teclado):", 11, 0);

    gpu_begin();
    gpu_clear(RGB(0, 0, 0));
    gpu_flip();
    gpu_end();

    while (1) {
        char c = kbd_getchar(); // bloqueante: espera el próximo carácter
        put_char(c);

        gpu_begin();
        gpu_flip(); // recompone el layer de texto sobre el FB
        gpu_end();
    }
}
