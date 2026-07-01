// abi.h — ABI de syscalls: la comparten el kernel y los programas de
// usuario (ver user/). Convención: `syscall` con $v0=número, $a0..$a3=hasta
// 4 argumentos, retorno en $v0. El kernel avanza EPC+4 al volver
// (kernel/trap.c::handle_syscall) para no reejecutar la instrucción.
#ifndef AIZ_ABI_H
#define AIZ_ABI_H

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3

static inline void sys_putc(char c) {
    register unsigned int r4 __asm__("$4") = (unsigned int)(unsigned char)c;
    register unsigned int r2 __asm__("$2") = SYS_PUTC;
    __asm__ volatile("syscall" : : "r"(r4), "r"(r2) : "memory");
}

// No bloqueante: -1 si no hay carácter disponible (ver SYS_GETC en trap.c).
static inline int sys_getc(void) {
    register unsigned int r2 __asm__("$2") = SYS_GETC;
    __asm__ volatile("syscall" : "+r"(r2) : : "memory");
    return (int)r2;
}

// El kernel reescribe el TrapFrame para volver directo al shell: esta
// syscall nunca retorna a quien la llamó.
static inline void sys_exit(void) {
    register unsigned int r2 __asm__("$2") = SYS_EXIT;
    __asm__ volatile("syscall" : : "r"(r2) : "memory");
    for (;;) {
    }
}

#endif // AIZ_ABI_H
