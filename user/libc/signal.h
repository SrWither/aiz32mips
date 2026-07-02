// signal.h — señales mínimas: sólo SIGINT (Ctrl+C) existe hoy, ver
// kernel/sched.c::sched_signal_fg. Si nunca llamás signal(), Ctrl+C mata el
// proceso como siempre (SIG_DFL es el default); si instalás un handler,
// corre él en vez de matarte.
#ifndef AIZ_LIBC_SIGNAL_H
#define AIZ_LIBC_SIGNAL_H

#include "../../kernel/abi.h" // SIGINT, SIG_DFL/SIG_IGN, sighandler_t, sys_signal/sys_sigreturn

// Trampolín de retorno: cuando tu handler termina (llega a su "}" y hace
// jr $ra), cae acá — el kernel puso esta dirección en $ra antes de
// saltarle (ver sched_signal_fg). sys_sigreturn() nunca vuelve de verdad:
// el kernel pisa el TrapFrame entero con el contexto guardado antes de la
// señal (ver sched_sigreturn), así que no importa qué prólogo/epílogo le
// agregue el compilador a esta función, ni qué haya "después" del syscall.
static inline void _sig_trampoline(void) {
    sys_sigreturn();
}

// signal(SIGINT, mi_handler): instala mi_handler(int signum), devuelve el
// handler anterior (o SIG_ERR si signum no es válido). signal(SIGINT,
// SIG_DFL) vuelve a matar el proceso; signal(SIGINT, SIG_IGN) lo ignora.
static inline sighandler_t signal(int signum, sighandler_t handler) {
    return sys_signal(signum, handler, (unsigned int)&_sig_trampoline);
}

#endif // AIZ_LIBC_SIGNAL_H
