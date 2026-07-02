// kernel.h — tipos y constantes compartidas entre boot.S y el C del kernel.
// Los offsets de TRAPFRAME_* tienen que coincidir exactamente con el orden
// de campos de `TrapFrame` de abajo: si cambiás uno, cambiá el otro.
#ifndef AIZ_KERNEL_H
#define AIZ_KERNEL_H

#ifndef __ASSEMBLER__
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
#include "abi.h"
#endif

// ───────────────────────────── CP0 ─────────────────────────────────────
#define STATUS_IE (1u << 0)
#define STATUS_EXL (1u << 1)
#define STATUS_ERL (1u << 2)
#define STATUS_KSU_MASK (0x3u << 3)
#define STATUS_KSU_USER (0x2u << 3) // Status.KSU=10: modo usuario
#define STATUS_IM2 (1u << 10) // teclado
#define STATUS_IM7 (1u << 15) // timer
#define STATUS_BEV (1u << 22)

#define CAUSE_EXCCODE_SHIFT 2
#define CAUSE_EXCCODE_MASK 0x7Cu
#define CAUSE_IP2 (1u << 10)
#define CAUSE_IP7 (1u << 15)

#define EXC_INT 0
#define EXC_MOD 1
#define EXC_TLBL 2
#define EXC_TLBS 3
#define EXC_ADEL 4
#define EXC_ADES 5
#define EXC_IBE 6
#define EXC_DBE 7
#define EXC_SYS 8
#define EXC_BP 9
#define EXC_RI 10
#define EXC_CPU 11
#define EXC_OV 12
#define EXC_TR 13

// Ciclos de Count entre interrupciones de timer.
#define TICK_PERIOD 100000

#ifndef __ASSEMBLER__

// Guardado por exception_entry (boot.S) antes de llamar a kernel_trap.
// El orden importa: tiene que coincidir con TRAPFRAME_OFF_* de boot.S.
typedef struct {
    u32 zero, at, v0, v1, a0, a1, a2, a3;
    u32 t0, t1, t2, t3, t4, t5, t6, t7;
    u32 s0, s1, s2, s3, s4, s5, s6, s7;
    u32 t8, t9, k0, k1, gp, sp, fp, ra;
    u32 hi, lo;
    u32 status, cause, epc, badvaddr;
} TrapFrame;

// ───────────────────────────── CP0 (inline) ────────────────────────────
static inline u32 cop0_read_status(void) {
    u32 v;
    __asm__ volatile("mfc0 %0, $12" : "=r"(v));
    return v;
}
static inline void cop0_write_status(u32 v) {
    __asm__ volatile("mtc0 %0, $12" : : "r"(v));
}
static inline u32 cop0_read_count(void) {
    u32 v;
    __asm__ volatile("mfc0 %0, $9" : "=r"(v));
    return v;
}
static inline void cop0_write_compare(u32 v) {
    __asm__ volatile("mtc0 %0, $11" : : "r"(v));
}
static inline void cop0_write_entryhi(u32 v) {
    __asm__ volatile("mtc0 %0, $10" : : "r"(v));
}
static inline void cop0_write_entrylo0(u32 v) {
    __asm__ volatile("mtc0 %0, $2" : : "r"(v));
}
static inline void cop0_write_entrylo1(u32 v) {
    __asm__ volatile("mtc0 %0, $3" : : "r"(v));
}
static inline void cop0_write_pagemask(u32 v) {
    __asm__ volatile("mtc0 %0, $5" : : "r"(v));
}
static inline void cop0_write_index(u32 v) {
    __asm__ volatile("mtc0 %0, $0" : : "r"(v));
}
static inline void cop0_tlbwi(void) {
    __asm__ volatile("tlbwi");
}
static inline void cop0_write_wired(u32 v) {
    __asm__ volatile("mtc0 %0, $6" : : "r"(v));
}
static inline u32 cop0_read_entryhi(void) {
    u32 v;
    __asm__ volatile("mfc0 %0, $10" : "=r"(v));
    return v;
}
// TLBWR: como tlbwi, pero a un índice "random" entre Wired y TLB_ENTRIES-1
// (el core lo recalcula a partir de Count, ver aiz32mips_core::cpu). La
// usa mm_handle_page_fault para el heap con demanda de páginas: a
// diferencia de prog/stack (wired, índice fijo por proceso), las páginas
// de heap comparten los índices no-wired entre todos los procesos y se
// van reemplazando.
static inline void cop0_tlbwr(void) {
    __asm__ volatile("tlbwr");
}
// TLBR: vuelca la entrada en Index hacia EntryHi/EntryLo0/EntryLo1/
// PageMask (para poder leerla con cop0_read_entryhi después). La usa
// mm_reset_heap para encontrar y tirar abajo entradas de un proceso que
// ya murió, antes de que su ASID lo reuse otro.
static inline void cop0_tlbr(void) {
    __asm__ volatile("tlbr");
}

// Tamaño real del TLB del core (aiz32mips_core::tlb::TLB_ENTRIES): no hay
// forma de leerlo en runtime, así que queda hardcodeado acá — si el core
// cambia esto alguna vez, hay que actualizarlo a mano.
#define TLB_ENTRIES 16

// console.c
void console_init(void);
void console_clear(void); // borra pantalla y vuelve el cursor a (0,0)
void console_putc(char c);
void console_puts(const char *s);
void console_put_hex(u32 v);
void console_put_uint(u32 v);
void console_flush(void); // recompone el texto sobre el FB (gpu_flip)

// trap.c
void kernel_trap(TrapFrame *tf);
void keyboard_irq(TrapFrame *tf);
void timer_tick(TrapFrame *tf);
int gpu_owned_by_user(void); // 1 si un proceso tiene la pantalla (ver sys_gpu_init)
// sched_signal_fg: libera la GPU sólo si el proceso que se mató por señal
// (acción default, sin handler instalado) era justo el dueño.
void gpu_release_if_owner(int pid);

// shell.c
void shell_init(void);
void shell_input(char c);

// Layout de vaddr de todo proceso de usuario (mm.c y sched.c lo comparten:
// un solo lugar para no desincronizar dónde arranca el programa, a qué
// vaddr apuntan los PT_LOAD del ELF, y dónde cae el heap que ve
// malloc()/free() en userland — ver user/libc/stdlib.h).
//
// Una sola entrada de TLB *wired* por proceso para texto+stack (índice ==
// ASID == slot, fijo desde sched_spawn/sched_fork, nunca se mueve). El
// heap es otra historia: es grande (16MB virtuales) pero se mapea de a
// una página de 4KB por vez, recién cuando el proceso la toca de verdad
// (TLB refill, ver mm_handle_page_fault) — para eso no hay wired de sobra
// (Wired=MAX_PROCS dedicados a texto+stack, el resto del TLB rota entre
// TODOS los procesos con tlbwr).
#define USER_VADDR 0x00400000u       // texto+datos+bss (una página, la del ELF)
#define USER_STACK_VADDR 0x00401000u // stack: una página, crece hacia abajo desde el tope (mismo VPN2 que prog)

#define USER_HEAP_VADDR 0x10000000u  // heap: región grande, con demanda de páginas (ver mm_handle_page_fault)
#define USER_HEAP_SIZE 0x01000000u   // 16MB virtuales — nada de esto es RAM real hasta que se toca

// Slots de proceso: 0 es el propio kernel/shell, 1..MAX_PROCS-1 son de
// usuario. Compartido entre sched.c (tabla de procesos) y mm.c (cada
// proceso usa 1 índice de TLB wired + su cupo del heap compartido) para
// que no haya que mantener el mismo número en dos lugares.
#define MAX_PROCS 4

// mm.c: memoria física + mecánica de TLB, sin política de proceso (eso es
// de sched.c).
u32 pmm_alloc_page(void);
void pmm_write_page(u32 phys, const u8 *data, u32 len);       // pisa la página entera (resto en 0)
void pmm_write_page_at(u32 phys, u32 offset, const u8 *data, u32 len); // solo ese rango, no toca el resto
void pmm_copy_page(u32 dst_phys, u32 src_phys);                        // la usa sched_fork
void mm_init(void);                                            // fija Wired al boot, antes de spawnear nada
void mm_map_user(u32 asid, u32 prog_phys, u32 stack_phys);      // entrada wired de texto+stack
int mm_handle_page_fault(u32 badvaddr); // TLB refill/inválida en el heap: 1 si se resolvió, 0 si no era del heap
void mm_reset_heap(u32 asid); // limpia la tabla de páginas + invalida entradas viejas de ese ASID (spawn/fork lo reusan)
void mm_fork_heap(u32 child_asid, u32 parent_asid); // copia las páginas de heap ya tocadas por el padre

// sched.c: scheduler round-robin. El proceso 0 es el propio kernel/shell.
// g_next_asid: el ASID del proceso elegido en el último switch; lo escribe
// exception_entry (boot.S) en EntryHi justo antes del eret, no sched.c —
// ver el comentario en sched.c para el por qué.
extern u32 g_next_asid;
void sched_init(void);
int sched_current_pid(void);
void sched_tick(TrapFrame *tf);       // preemption: la llama timer_tick
void sched_block_current(TrapFrame *tf); // duerme al actual (sem_wait)
void sched_wake(int pid);                // lo despierta (sem_signal)
int sched_spawn(const char *path, u32 arg0); // lee+parsea el ELF de disco; slot o -1
int sched_fork(TrapFrame *tf); // duplica al proceso actual; pid del hijo (o -1) para el padre
void sched_exit_current(TrapFrame *tf);      // usan SYS_EXIT y el kill por fallo
void sched_kill_pid(TrapFrame *tf, int pid); // termina un pid puntual (acción default de una señal)

// Señales (ver sched.c y user/libc/signal.h): sólo SIGINT hoy, entregada al
// proceso en foreground (el último que arrancó sched_spawn) en vez de
// matarlos a todos como antes.
int sched_signal_fg(TrapFrame *tf, int signum); // Ctrl+C; devuelve el pid matado por acción default, o 0
u32 sched_set_sig_handler(int pid, int signum, u32 handler, u32 trampoline); // SYS_SIGNAL
void sched_sigreturn(TrapFrame *tf, int pid);                               // SYS_SIGRETURN

// sem.c: semáforos de Dijkstra (wait/signal), bloqueantes de verdad — sin
// busy-wait, sched_block_current/sched_wake sacan y devuelven al proceso
// de la rotación del scheduler.
void sem_init(int idx, int value);
int sem_wait(TrapFrame *tf, int idx);    // devuelve 1 si bloqueó
void sem_signal(int idx);

// fs.c: FAT32 sobre storage.h (montaje perezoso: recién toca el device en
// el primer uso). Solo nombres cortos 8.3. Sin cwd propio acá adentro:
// shell.c arma paths absolutos antes de llamar a estas.
int fs_read(const char *path, u8 *buf, u32 maxlen); // bytes leídos, -1 si no existe o es directorio
int fs_read_at(const char *path, u32 offset, u8 *buf, u32 maxlen); // idem, pero arrancando en offset
int fs_write(const char *path, const u8 *data, u32 len); // crea/trunca; 0 ok, -1 si no se pudo
void fs_list(const char *path);                     // imprime error si el path no es un directorio
int fs_is_dir(const char *path);                     // 1/0/-1 (no existe), para cd

// fs.c: descriptores de archivo para syscalls de userland (SYS_OPEN/READ/
// WRITE/CLOSE, ver trap.c). fd >= FD_BASE(3); -1 en cualquier error.
int fs_open(const char *path, int write_mode, int owner_pid);
int fs_fd_read(int fd, u8 *buf, u32 maxlen);
int fs_fd_write(int fd, const u8 *buf, u32 len);
int fs_fd_close(int fd);
void fs_close_all_owned_by(int pid); // la llaman sched_exit_current/sched_kill_pid

// kernel.c
void kernel_main(void);
void kpanic(TrapFrame *tf, const char *msg);
const char *exc_name(u32 code);

#endif // __ASSEMBLER__

#endif // AIZ_KERNEL_H
