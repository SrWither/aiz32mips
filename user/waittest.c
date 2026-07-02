// waittest.c — prueba SYS_WAIT (ver kernel/sched.c::sched_wait_for +
// libc/unistd.h::wait()): el padre bloquea hasta que el hijo termine, a
// diferencia de pingpong.c (que corre los dos en paralelo, sincronizados
// sólo por semáforos). Si wait() no bloqueara de verdad, "padre" podría
// imprimir su mensaje ANTES que el hijo — waittest lo verifica de forma
// determinística viendo el pid que devuelve wait(), no sólo el orden de
// la salida (que ya sería una señal fuerte, pero el pid es la prueba
// real de que esperó al proceso correcto).
#include "libc/stdio.h"
#include "libc/unistd.h"

void _start(void) {
    int pid = fork();
    if (pid == 0) {
        // hijo: hace algo de trabajo (visible en la consola) y termina.
        for (int i = 0; i < 3; i++) {
            printf(".");
        }
        printf(" hijo: termine\n");
        sys_exit();
    }

    // padre: bloquea hasta que el hijo (pid > 0 acá) termine.
    int waited = wait(pid);
    if (waited == pid) {
        printf("padre: wait ok, hijo %d termino\n", pid);
    } else {
        printf("padre: wait fallo (devolvio %d, esperaba %d)\n", waited, pid);
    }
    sys_exit();
}
