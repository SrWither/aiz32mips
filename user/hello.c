// hello.c — primera app de usuario: prueba de punta a punta el pipeline
// modo kernel/usuario + la ABI de syscalls (ver ../kernel/abi.h). Sin
// boot.S propio: el $sp inicial lo arma el kernel antes del eret
// (kernel/mm.c::user_map_and_load + boot.S::enter_user_mode).
#include "../kernel/abi.h"

void _start(void) {
    const char *msg = "hello from user mode!\n";
    for (const char *p = msg; *p; p++) {
        sys_putc(*p);
    }
    sys_exit();
}
