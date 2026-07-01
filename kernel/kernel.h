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
#endif

// ───────────────────────────── CP0 ─────────────────────────────────────
#define STATUS_IE (1u << 0)
#define STATUS_EXL (1u << 1)
#define STATUS_ERL (1u << 2)
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

// ───────────────────────────── syscalls ────────────────────────────────
#define SYS_PUTC 1
#define SYS_GETC 2
#define SYS_EXIT 3

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

// console.c
void console_init(void);
void console_putc(char c);
void console_puts(const char *s);
void console_put_hex(u32 v);
void console_put_uint(u32 v);
void console_flush(void); // recompone el texto sobre el FB (gpu_flip)

// trap.c
void kernel_trap(TrapFrame *tf);
void keyboard_irq(void);
void timer_tick(void);

// kernel.c
void kernel_main(void);
void kpanic(TrapFrame *tf, const char *msg);

#endif // __ASSEMBLER__

#endif // AIZ_KERNEL_H
