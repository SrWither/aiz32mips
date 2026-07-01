// abi.h — ABI de syscalls: la comparten el kernel y los programas de
// usuario (ver user/). Convención: `syscall` con $v0=número, $a0..$a3=hasta
// 4 argumentos, retorno en $v0. El kernel avanza EPC+4 al volver
// (kernel/trap.c::handle_syscall) para no reejecutar la instrucción.
#ifndef AIZ_ABI_H
#define AIZ_ABI_H

#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3
#define SYS_SEM_WAIT 4
#define SYS_SEM_SIGNAL 5
// GPU: userland no tiene acceso directo a VRAM/MMIO (viven en kseg1, fuera
// de kuseg — sin eso no hay TLB posible), así que la única forma de
// dibujar es que el kernel medie. sys_gpu_submit manda una display list ya
// armada en un buffer del propio proceso (kuseg, mapeado por su TLB): el
// kernel la lee de ahí con el ASID todavía puesto (nunca cambia entre el
// syscall y su handler) y la copia a VRAM_CMDBUF. Ver user/gpu_user.h.
#define SYS_GPU_INIT 6
#define SYS_GPU_SUBMIT 7
#define SYS_GPU_STATUS 8
#define SYS_FORK 9

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

// Bloquea (sin busy-wait: el kernel saca al proceso de la rotación del
// scheduler) hasta que otro proceso haga sys_sem_signal(idx).
static inline void sys_sem_wait(int idx) {
    register unsigned int r4 __asm__("$4") = (unsigned int)idx;
    register unsigned int r2 __asm__("$2") = SYS_SEM_WAIT;
    __asm__ volatile("syscall" : : "r"(r4), "r"(r2) : "memory");
}

static inline void sys_sem_signal(int idx) {
    register unsigned int r4 __asm__("$4") = (unsigned int)idx;
    register unsigned int r2 __asm__("$2") = SYS_SEM_SIGNAL;
    __asm__ volatile("syscall" : : "r"(r4), "r"(r2) : "memory");
}

static inline void sys_gpu_init(unsigned int w, unsigned int h, unsigned int double_buffer,
                                 unsigned int with_zbuffer) {
    register unsigned int r4 __asm__("$4") = w;
    register unsigned int r5 __asm__("$5") = h;
    register unsigned int r6 __asm__("$6") = double_buffer;
    register unsigned int r7 __asm__("$7") = with_zbuffer;
    register unsigned int r2 __asm__("$2") = SYS_GPU_INIT;
    __asm__ volatile("syscall" : : "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r2) : "memory");
}

// buf: puntero del propio proceso (kuseg) a n_words de u32 con la display
// list ya armada (mismos opcodes que gpu.h). El kernel la copia a
// VRAM_CMDBUF y la ejecuta (equivalente a gpu_end() en modo kernel).
static inline void sys_gpu_submit(const void *buf, unsigned int n_words) {
    register unsigned int r4 __asm__("$4") = (unsigned int)buf;
    register unsigned int r5 __asm__("$5") = n_words;
    register unsigned int r2 __asm__("$2") = SYS_GPU_SUBMIT;
    __asm__ volatile("syscall" : : "r"(r4), "r"(r5), "r"(r2) : "memory");
}

// Devuelve REG_STATUS tal cual (bit1 = VBLANK, ver gpu.h): sin esto no hay
// forma de esperar el vsync desde userland.
static inline unsigned int sys_gpu_status(void) {
    register unsigned int r2 __asm__("$2") = SYS_GPU_STATUS;
    __asm__ volatile("syscall" : "+r"(r2) : : "memory");
    return r2;
}

// fork(): duplica al proceso actual entero (las 4 páginas físicas, ver
// kernel/sched.c::sched_fork) en un slot nuevo. Devuelve el pid del hijo
// en el padre, 0 en el hijo, -1 si no había slot libre — igual que el
// fork() de toda la vida, aunque acá adentro es copia completa de página,
// no copy-on-write.
static inline int sys_fork(void) {
    register unsigned int r2 __asm__("$2") = SYS_FORK;
    __asm__ volatile("syscall" : "+r"(r2) : : "memory");
    return (int)r2;
}

#endif // AIZ_ABI_H
