// pingpong.c — demo de semáforos (ver ../kernel/sem.c) Y de fork() (ver
// libc/unistd.h y kernel/sched.c::sched_fork): un solo "pingpong" alcanza
// — se clona a sí mismo, padre e hijo quedan distinguidos por el valor de
// retorno de fork() igual que en cualquier Unix de verdad. Rendezvous
// clásico de dos semáforos (0 y 1, arrancan en 0): fuerza una alternancia
// estricta ".o.o.o.o.o" que solo puede darse si el bloqueo/despertar
// funciona de verdad.
//
// Sin sem_init explícito: arrancan en 0 por el zero-init de .bss al
// bootear, y una corrida completa (5 iteraciones de cada rol) deja los dos
// semáforos otra vez en 0, así que se puede repetir sin reinicializar. Si
// se corta a mitad de camino (Ctrl+C) los contadores pueden quedar
// desalineados para la corrida siguiente — no hay reset automático para
// ese caso todavía.
#include "../kernel/abi.h"
#include "libc/unistd.h"

void _start(void) {
    int role = (fork() == 0) ? 1 : 0; // el hijo (fork()==0) hace de "o"

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
