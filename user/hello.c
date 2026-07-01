// hello.c — primera app de usuario: prueba de punta a punta el pipeline
// modo kernel/usuario + la ABI de syscalls (ver ../kernel/abi.h). Sin
// boot.S propio: el $sp inicial lo arma el kernel (kernel/sched.c::sched_spawn)
// antes de que el scheduler la haga correr por primera vez. Usa libc/stdio.h
// en vez de sys_putc directo, para probar que la libc mínima funciona.
#include "libc/stdio.h"
#include "libc/stdlib.h"

void _start(void) {
    printf("hello from user mode!\n");
    exit(0);
}
