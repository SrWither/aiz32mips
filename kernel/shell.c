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
    // No bloquea: sched_spawn solo registra el proceso, el scheduler lo
    // hace correr desde el próximo tick del timer (ver sched.c).
    if (sched_spawn(_binary_hello_bin_start, len, 0) < 0) {
        console_puts("no hay slots de proceso libres\n");
    }
}

static void cmd_pingpong(void) {
    // Semáforos en (0,0): arrancan "vacíos", el primer sys_sem_wait de
    // cada rol bloquea hasta que el otro haga su sys_sem_signal (ver
    // user/pingpong.c y kernel/sem.c).
    sem_init(0, 0);
    sem_init(1, 0);
    u32 len = (u32)(_binary_pingpong_bin_end - _binary_pingpong_bin_start);
    if (sched_spawn(_binary_pingpong_bin_start, len, 0) < 0 ||
        sched_spawn(_binary_pingpong_bin_start, len, 1) < 0) {
        console_puts("no hay slots de proceso libres\n");
    }
}

static void shell_dispatch(void) {
    cmd_buf[cmd_len] = 0;
    if (cmd_len == 0) {
        // linea vacia: no hace nada
    } else if (str_eq(cmd_buf, "ping")) {
        cmd_ping();
    } else if (str_eq(cmd_buf, "run")) {
        cmd_run();
    } else if (str_eq(cmd_buf, "pingpong")) {
        cmd_pingpong();
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
