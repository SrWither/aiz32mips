// pingpong.c — demo de semáforos (ver ../kernel/sem.c): dos instancias de
// este mismo binario, distinguidas por "rol" (kernel/sched.c::sched_spawn
// deja el 3er argumento en $a0, así que _start(int role) lo recibe como
// cualquier función C — ABI o32 estándar, sin nada especial). Rendezvous
// clásico de dos semáforos (0 y 1, arrancan en 0): fuerza una alternancia
// estricta ".o.o.o.o.o" que solo puede darse si el bloqueo/despertar
// funciona de verdad.
#include "../kernel/abi.h"

void _start(int role) {
    for (int i = 0; i < 5; i++) {
        if (role == 0) {
            sys_sem_signal(0);
            sys_sem_wait(1);
            sys_putc('.');
        } else {
            sys_sem_wait(0);
            sys_sem_signal(1);
            sys_putc('o');
        }
    }
    sys_exit();
}
