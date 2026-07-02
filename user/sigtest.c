// sigtest.c — prueba de señales de usuario: instala un handler para
// SIGINT (ver kernel/sched.c::sched_signal_fg + user/libc/signal.h) y
// demuestra que Ctrl+C lo corre a él en vez de matar el proceso, y que
// después de sys_sigreturn (dentro de signal(), la llama _sig_trampoline)
// se sigue ejecutando exactamente donde se había interrumpido.
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/signal.h"

static volatile int got_sigint = 0;

static void on_sigint(int signum) {
    (void)signum;
    got_sigint = 1;
}

void _start(void) {
    signal(SIGINT, on_sigint);
    printf("sigtest: esperando Ctrl+C...\n");
    while (!got_sigint) {
        // busy-wait: no hay sys_sleep todavía, esto alcanza para la prueba
    }
    printf("sigtest: SIGINT atendido con handler, sigo vivo\n");
    exit(0);
}
