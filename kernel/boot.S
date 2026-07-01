// boot.S — entrada de reset + vectores de excepción + trampolín de trap.
//
// Todo el kernel vive en kseg1 (0xBFC00000+): nunca tocamos Status.BEV,
// así que los vectores quedan en las direcciones "de ROM" de siempre
// (0xBFC00180 general, 0xBFC00200 TLB refill) en vez de moverse a RAM en
// 0x80000180 — más simple para un kernel que todavía no usa TLB de verdad.
//
// El linker script (linker.ld) es quien pone .text.reset/.vector_general/
// .vector_tlb exactamente en esas tres direcciones.

#include "kernel.h"

.set noreorder
.set noat

.global _start
.global exception_entry

#define TF_SIZE     152
#define TF_AT       4
#define TF_V0       8
#define TF_V1       12
#define TF_A0       16
#define TF_A1       20
#define TF_A2       24
#define TF_A3       28
#define TF_T0       32
#define TF_T1       36
#define TF_T2       40
#define TF_T3       44
#define TF_T4       48
#define TF_T5       52
#define TF_T6       56
#define TF_T7       60
#define TF_S0       64
#define TF_S1       68
#define TF_S2       72
#define TF_S3       76
#define TF_S4       80
#define TF_S5       84
#define TF_S6       88
#define TF_S7       92
#define TF_T8       96
#define TF_T9       100
#define TF_K0       104
#define TF_K1       108
#define TF_GP       112
#define TF_SP       116
#define TF_FP       120
#define TF_RA       124
#define TF_HI       128
#define TF_LO       132
#define TF_STATUS   136
#define TF_CAUSE    140
#define TF_EPC      144
#define TF_BADVADDR 148

// ───────────────────────────── reset ───────────────────────────────────
.section .text.reset, "ax"
_start:
    la    $sp, _stack_top
    j     kernel_main
    nop

// ───────────────────────────── vectores ────────────────────────────────
.section .text.vector_general, "ax"
vector_general:
    j     exception_entry
    nop

.section .text.vector_tlb, "ax"
vector_tlb:
    j     exception_entry
    nop

// ───────────────────────────── trampolín de trap ───────────────────────
// Guarda el contexto completo, llama a kernel_trap(TrapFrame*) en C, y
// restaura antes de eret. $sp se reutiliza tal cual estaba (sin stack de
// excepción separado): alcanza mientras el kernel nunca haga overflow de
// su propio stack dentro de un handler.
.section .text
exception_entry:
    addiu $sp, $sp, -TF_SIZE

    sw    $1,  TF_AT($sp)
    sw    $2,  TF_V0($sp)
    sw    $3,  TF_V1($sp)
    sw    $4,  TF_A0($sp)
    sw    $5,  TF_A1($sp)
    sw    $6,  TF_A2($sp)
    sw    $7,  TF_A3($sp)
    sw    $8,  TF_T0($sp)
    sw    $9,  TF_T1($sp)
    sw    $10, TF_T2($sp)
    sw    $11, TF_T3($sp)
    sw    $12, TF_T4($sp)
    sw    $13, TF_T5($sp)
    sw    $14, TF_T6($sp)
    sw    $15, TF_T7($sp)
    sw    $16, TF_S0($sp)
    sw    $17, TF_S1($sp)
    sw    $18, TF_S2($sp)
    sw    $19, TF_S3($sp)
    sw    $20, TF_S4($sp)
    sw    $21, TF_S5($sp)
    sw    $22, TF_S6($sp)
    sw    $23, TF_S7($sp)
    sw    $24, TF_T8($sp)
    sw    $25, TF_T9($sp)
    sw    $26, TF_K0($sp)
    sw    $27, TF_K1($sp)
    sw    $28, TF_GP($sp)
    sw    $30, TF_FP($sp)
    sw    $31, TF_RA($sp)

    addiu $2, $sp, TF_SIZE   // $sp original (antes de reservar el frame)
    sw    $2, TF_SP($sp)

    mfhi  $2
    sw    $2, TF_HI($sp)
    mflo  $2
    sw    $2, TF_LO($sp)

    mfc0  $2, $12            // Status
    sw    $2, TF_STATUS($sp)
    mfc0  $2, $13            // Cause
    sw    $2, TF_CAUSE($sp)
    mfc0  $2, $14            // EPC
    sw    $2, TF_EPC($sp)
    mfc0  $2, $8             // BadVAddr
    sw    $2, TF_BADVADDR($sp)

    move  $4, $sp
    jal   kernel_trap
    nop

    // Status/EPC los puede haber tocado kernel_trap (p.ej. avanzar EPC
    // tras un syscall), así que se restauran desde el frame, no desde CP0.
    lw    $2, TF_STATUS($sp)
    mtc0  $2, $12
    lw    $2, TF_EPC($sp)
    mtc0  $2, $14
    lw    $2, TF_HI($sp)
    mthi  $2
    lw    $2, TF_LO($sp)
    mtlo  $2

    lw    $1,  TF_AT($sp)
    lw    $2,  TF_V0($sp)
    lw    $3,  TF_V1($sp)
    lw    $4,  TF_A0($sp)
    lw    $5,  TF_A1($sp)
    lw    $6,  TF_A2($sp)
    lw    $7,  TF_A3($sp)
    lw    $8,  TF_T0($sp)
    lw    $9,  TF_T1($sp)
    lw    $10, TF_T2($sp)
    lw    $11, TF_T3($sp)
    lw    $12, TF_T4($sp)
    lw    $13, TF_T5($sp)
    lw    $14, TF_T6($sp)
    lw    $15, TF_T7($sp)
    lw    $16, TF_S0($sp)
    lw    $17, TF_S1($sp)
    lw    $18, TF_S2($sp)
    lw    $19, TF_S3($sp)
    lw    $20, TF_S4($sp)
    lw    $21, TF_S5($sp)
    lw    $22, TF_S6($sp)
    lw    $23, TF_S7($sp)
    lw    $24, TF_T8($sp)
    lw    $25, TF_T9($sp)
    lw    $26, TF_K0($sp)
    lw    $27, TF_K1($sp)
    lw    $28, TF_GP($sp)
    lw    $30, TF_FP($sp)
    lw    $31, TF_RA($sp)

    // $sp sale del frame, no de un "addiu ,TF_SIZE": para el caso normal
    // da lo mismo (TF_SP se guardó como sp_original = sp_actual+TF_SIZE en
    // el prólogo), pero así SYS_EXIT (trap.c) puede redirigir el $sp real
    // con solo pisar tf->sp, sin este load quedaría clobbereado sin efecto.
    lw    $29, TF_SP($sp)

    // El ASID del proceso elegido (sched.c) se escribe recién ACÁ, después
    // de la última lectura vía $sp: si se escribiera antes (p.ej. desde C,
    // en sched_switch_to), todos los "jr $ra" de vuelta por
    // sched_switch_to/sched_tick/handle_syscall/kernel_trap seguirían
    // usando el stack de kuseg del proceso VIEJO, pero ya traducido contra
    // la página del proceso NUEVO — corrompiendo el desenrolle. $k1 se pisa
    // a propósito: por convención MIPS es de uso reservado del SO, ningún
    // código debería depender de que sobreviva un cambio de contexto.
    la    $27, g_next_asid
    lw    $27, 0($27)
    mtc0  $27, $10

    eret
