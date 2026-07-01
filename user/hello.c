// hello.c — primera app de usuario: prueba de punta a punta el pipeline
// modo kernel/usuario + la ABI de syscalls (ver ../kernel/abi.h). Sin
// boot.S propio: el $sp inicial lo arma el kernel (kernel/sched.c::sched_spawn)
// antes de que el scheduler la haga correr por primera vez.
#include "../kernel/abi.h"

void _start(void) {
    const char *msg = "hello from user mode!\n";
    for (const char *p = msg; *p; p++) {
        sys_putc(*p);
    }
    sys_exit();
}
