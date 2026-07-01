// shell.c — línea de comandos simple: junta lo que se tipea y lo despacha
// al apretar Enter. Corre en contexto de keyboard_irq (IRQ), no hay
// syscalls acá: se escribe directo a la consola.
#include "kernel.h"
#include "embedded.h"

#define CMD_MAX 40

static char cmd_buf[CMD_MAX];
static int cmd_len = 0;

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void cmd_ping(void) {
    console_puts("pong\n");
}

static void cmd_run(void) {
    u32 len = (u32)(_binary_hello_bin_end - _binary_hello_bin_start);
    user_map_and_load(_binary_hello_bin_start, len);
    enter_user_mode(0x00400000u, 0x00402000u);
}

static void shell_dispatch(void) {
    cmd_buf[cmd_len] = 0;
    if (cmd_len == 0) {
        // linea vacia: no hace nada
    } else if (str_eq(cmd_buf, "ping")) {
        cmd_ping();
    } else if (str_eq(cmd_buf, "run")) {
        cmd_run();
    } else {
        console_puts("comando desconocido: ");
        console_puts(cmd_buf);
        console_putc('\n');
    }
    cmd_len = 0;
}

void shell_init(void) {
    cmd_len = 0;
}

void shell_input(char c) {
    if (c == '\n' || c == '\r') {
        console_putc('\n');
        shell_dispatch();
        console_puts("> ");
        return;
    }
    if (c == 8 || c == 127) { // backspace/delete
        if (cmd_len > 0) {
            cmd_len--;
            console_putc(8); // console.c borra la ultima celda y retrocede
        }
        return;
    }
    if (c < 32 || c > 126) {
        return; // resto de codigos de control: se ignoran
    }
    if (cmd_len < CMD_MAX - 1) {
        cmd_buf[cmd_len++] = c;
        console_putc(c);
    }
}
