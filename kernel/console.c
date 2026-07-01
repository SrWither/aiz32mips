// console.c — consola de texto del kernel, arriba del modo texto por
// hardware de la GPU (ver aiz32mips_emu/data/gpu.h).
#include "gpu.h"
#include "kernel.h"

#define CON_COLS 40
#define CON_ROWS 25

static int cur_col = 0;
static int cur_row = 0;

static void con_newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= CON_ROWS) {
        // sin scroll real todavía: al llegar abajo, arranca de nuevo
        cur_row = 0;
        gpu_text_clear();
    }
}

void console_init(void) {
    gpu_init(320, 200, 0, 0);
    gpu_text_init(CON_COLS, CON_ROWS);
    gpu_text_clear();

    gpu_begin();
    gpu_clear(RGB(0, 0, 0));
    gpu_flip();
    gpu_end();
}

void console_putc(char c) {
    if (c == '\n' || c == '\r') {
        con_newline();
        return;
    }
    if (c == 8 || c == 127) { // backspace / delete
        if (cur_col > 0) {
            cur_col--;
        } else if (cur_row > 0) {
            cur_row--;
            cur_col = CON_COLS - 1;
        }
        gpu_text_putc(cur_col, cur_row, ' ', 15, 0);
        return;
    }
    if (c < 32 || c > 126) {
        return;
    }
    gpu_text_putc(cur_col, cur_row, c, 15, 0);
    cur_col++;
    if (cur_col >= CON_COLS) {
        con_newline();
    }
}

void console_puts(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}

void console_put_hex(u32 v) {
    console_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        u32 nib = (v >> shift) & 0xF;
        console_putc(nib < 10 ? (char)('0' + nib) : (char)('A' + nib - 10));
    }
}

void console_put_uint(u32 v) {
    char buf[10];
    int n = 0;
    if (v == 0) {
        console_putc('0');
        return;
    }
    while (v > 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        console_putc(buf[--n]);
    }
}

void console_flush(void) {
    gpu_begin();
    gpu_flip();
    gpu_end();
}
