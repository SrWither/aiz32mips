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
// Archivos: fd >= 3 (0/1/2 quedan reservados para un futuro stdin/stdout/
// stderr, hoy sin usar — la consola sigue yendo por SYS_PUTC/SYS_GETC).
// Ver kernel/fs.c: la tabla de descriptores y fs_open/fs_fd_*.
#define SYS_OPEN 10
#define SYS_READ 11
#define SYS_WRITE 12
#define SYS_CLOSE 13
// Señales: sólo SIGINT existe hoy (Ctrl+C, ver kernel/trap.c::keyboard_irq
// + kernel/sched.c::sched_signal_fg). Numeración estilo POSIX para que sea
// familiar, aunque acá no hay ni remotamente el resto del set.
#define SYS_SIGNAL 14
#define SYS_SIGRETURN 15

#define SIGINT 2
#define NSIG 8 // tope de sched.c::Process.sig_handler; deja lugar para crecer

// Mueve el cursor de lectura de un fd ya abierto (ver kernel/fs.c::
// fs_fd_seek): sólo para fds de lectura, los de escritura son un buffer de
// acumulación sin noción de posición (ver fs_fd_write/unistd.h).
#define SYS_LSEEK 16

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Audio: un canal PCM mono s16 a AUDIO_SAMPLE_RATE fijo (ver
// kernel/audio.h). Igual que la GPU, userland no tiene acceso directo al
// MMIO (kseg1, sin TLB) — sys_audio_submit copia el buffer del proceso al
// staging del device y dispara KICK, troceando si hace falta.
#define SYS_AUDIO_SUBMIT 17
#define SYS_AUDIO_STATUS 18

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

// write_mode: 0 = lectura (el archivo tiene que existir), 1 = escritura
// (crea/trunca recién al hacer sys_close, ver kernel/fs.c). Devuelve un
// fd (>=3) o -1.
static inline int sys_open(const char *path, int write_mode) {
    register unsigned int r4 __asm__("$4") = (unsigned int)path;
    register unsigned int r5 __asm__("$5") = (unsigned int)write_mode;
    register unsigned int r2 __asm__("$2") = SYS_OPEN;
    __asm__ volatile("syscall" : "+r"(r2) : "r"(r4), "r"(r5) : "memory");
    return (int)r2;
}

static inline int sys_read(int fd, void *buf, unsigned int len) {
    register unsigned int r4 __asm__("$4") = (unsigned int)fd;
    register unsigned int r5 __asm__("$5") = (unsigned int)buf;
    register unsigned int r6 __asm__("$6") = len;
    register unsigned int r2 __asm__("$2") = SYS_READ;
    __asm__ volatile("syscall" : "+r"(r2) : "r"(r4), "r"(r5), "r"(r6) : "memory");
    return (int)r2;
}

// Escritura "parcial" al estilo write(2) real: puede devolver menos de
// `len` si el buffer interno del fd (FS_WRITE_BUF_SIZE, ver fs.c) no
// entra todo.
static inline int sys_write(int fd, const void *buf, unsigned int len) {
    register unsigned int r4 __asm__("$4") = (unsigned int)fd;
    register unsigned int r5 __asm__("$5") = (unsigned int)buf;
    register unsigned int r6 __asm__("$6") = len;
    register unsigned int r2 __asm__("$2") = SYS_WRITE;
    __asm__ volatile("syscall" : "+r"(r2) : "r"(r4), "r"(r5), "r"(r6) : "memory");
    return (int)r2;
}

// Si el fd se abrió para escribir, recién acá se vuelca de verdad al
// disco (ver fs_fd_close).
static inline int sys_close(int fd) {
    register unsigned int r4 __asm__("$4") = (unsigned int)fd;
    register unsigned int r2 __asm__("$2") = SYS_CLOSE;
    __asm__ volatile("syscall" : "+r"(r2) : "r"(r4) : "memory");
    return (int)r2;
}

// whence: SEEK_SET/SEEK_CUR/SEEK_END. Devuelve la nueva posición o -1 (fd
// de escritura, o resultado fuera de [0, tamaño del archivo]).
static inline int sys_lseek(int fd, int offset, int whence) {
    register unsigned int r4 __asm__("$4") = (unsigned int)fd;
    register unsigned int r5 __asm__("$5") = (unsigned int)offset;
    register unsigned int r6 __asm__("$6") = (unsigned int)whence;
    register unsigned int r2 __asm__("$2") = SYS_LSEEK;
    __asm__ volatile("syscall" : "+r"(r2) : "r"(r4), "r"(r5), "r"(r6) : "memory");
    return (int)r2;
}

// Somete hasta `n` samples s16 (mono, AUDIO_SAMPLE_RATE) para reproducir,
// en el orden dado. Sin retorno de "cuántos entraron": el kernel siempre
// los encola todos (troceando en tandas de AUDIO_BUF_SAMPLES si hace
// falta, ver kernel/trap.c) — sys_audio_status() es la forma de no
// mandar de más si no hace falta.
static inline void sys_audio_submit(const short *samples, unsigned int n) {
    register unsigned int r4 __asm__("$4") = (unsigned int)samples;
    register unsigned int r5 __asm__("$5") = n;
    register unsigned int r2 __asm__("$2") = SYS_AUDIO_SUBMIT;
    __asm__ volatile("syscall" : : "r"(r4), "r"(r5), "r"(r2) : "memory");
}

// Samples todavía sin reproducir en la cola del device (REG_AUDIO_STATUS).
static inline unsigned int sys_audio_status(void) {
    register unsigned int r2 __asm__("$2") = SYS_AUDIO_STATUS;
    __asm__ volatile("syscall" : "+r"(r2) : : "memory");
    return r2;
}

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)  // acción por default: termina el proceso
#define SIG_IGN ((sighandler_t)1)  // ignora la señal
#define SIG_ERR ((sighandler_t)-1) // sys_signal: signum inválido

// Instala `handler` para `signum` (SIG_DFL/SIG_IGN o una función propia,
// ver user/libc/signal.h) y devuelve el handler anterior (o SIG_ERR si
// signum es inválido). `trampoline` es la dirección de retorno que arma el
// kernel en el TrapFrame antes de saltar al handler: cuando este vuelve
// (jr $ra) cae ahí, y ese código sólo tiene que hacer sys_sigreturn() — ver
// user/libc/signal.h::_sig_trampoline. Sin esto el kernel no tendría forma
// de saber a qué dirección de USUARIO volver.
static inline sighandler_t sys_signal(int signum, sighandler_t handler, unsigned int trampoline) {
    register unsigned int r4 __asm__("$4") = (unsigned int)signum;
    register unsigned int r5 __asm__("$5") = (unsigned int)handler;
    register unsigned int r6 __asm__("$6") = trampoline;
    register unsigned int r2 __asm__("$2") = SYS_SIGNAL;
    __asm__ volatile("syscall" : "+r"(r2) : "r"(r4), "r"(r5), "r"(r6) : "memory");
    return (sighandler_t)r2;
}

// La llama _sig_trampoline (user/libc/signal.h) al volver de un handler: el
// kernel restaura el TrapFrame completo previo a la señal y no vuelve nunca
// a quien la invocó (mismo motivo que sys_exit: no hay "después" acá).
static inline void sys_sigreturn(void) {
    register unsigned int r2 __asm__("$2") = SYS_SIGRETURN;
    __asm__ volatile("syscall" : : "r"(r2) : "memory");
    for (;;) {
    }
}

#endif // AIZ_ABI_H
