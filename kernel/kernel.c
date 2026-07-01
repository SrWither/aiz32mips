// kernel.c — arranque del kernel: consola, timer, teclado, panic.
#include "gpu.h"
#include "keyboard.h"
#include "kernel.h"

const char *exc_name(u32 code) {
    switch (code) {
        case EXC_INT: return "Int";
        case EXC_MOD: return "Mod";
        case EXC_TLBL: return "TLBL";
        case EXC_TLBS: return "TLBS";
        case EXC_ADEL: return "AdEL";
        case EXC_ADES: return "AdES";
        case EXC_IBE: return "IBE";
        case EXC_DBE: return "DBE";
        case EXC_SYS: return "Sys";
        case EXC_BP: return "Bp";
        case EXC_RI: return "RI";
        case EXC_CPU: return "CpU";
        case EXC_OV: return "Ov";
        case EXC_TR: return "Tr";
        default: return "?";
    }
}

void kpanic(TrapFrame *tf, const char *msg) {
    // Corta interrupciones: no queremos otra excepción mientras mostramos
    // el panic (no hay re-entrancia contemplada en exception_entry).
    cop0_write_status(STATUS_BEV);

    u32 exc_code = (tf->cause & CAUSE_EXCCODE_MASK) >> CAUSE_EXCCODE_SHIFT;

    console_putc('\n');
    console_puts("*** KERNEL PANIC ***\n");
    console_puts(msg);
    console_putc('\n');
    console_puts("ExcCode:  ");
    console_puts(exc_name(exc_code));
    console_putc('\n');
    console_puts("EPC:      ");
    console_put_hex(tf->epc);
    console_putc('\n');
    console_puts("BadVAddr: ");
    console_put_hex(tf->badvaddr);
    console_putc('\n');
    console_flush();

    for (;;) {
        // sin scheduler: acá se cuelga el sistema
    }
}

void kernel_main(void) {
    console_init();

    // El reset deja Status.ERL=1 (aiz32mips_core::cpu::CPU::reset()); hay
    // que bajarlo antes de la primera excepción, si no ERET usaría
    // ErrorEPC en vez de EPC. BEV se mantiene en 1 a propósito: los
    // vectores de este kernel viven en kseg1 (ver linker.ld).
    cop0_write_status(STATUS_BEV);

    console_puts("AIZ-32 mini kernel\n");
    console_puts("excepciones + timer (IP7) + teclado (IP2) por IRQ\n\n");

    // proceso 0 = este mismo kernel/shell; se registra antes de habilitar
    // interrupciones para que el primer tick del timer ya tenga a quién
    // guardar el contexto.
    sched_init();

    // arranca el timer: primer tick en TICK_PERIOD ciclos de Count
    cop0_write_compare(cop0_read_count() + TICK_PERIOD);

    // el device de teclado empieza a reflejar su FIFO en Cause.IP2
    kbd_enable_irq();

    // habilita interrupciones: IE + IM2 (teclado) + IM7 (timer)
    cop0_write_status(STATUS_BEV | STATUS_IE | STATUS_IM2 | STATUS_IM7);

    shell_init();
    // sys_putc (no console_putc directo): el primer prompt "> " sirve de
    // prueba end-to-end de todo el pipeline de excepciones: SYSCALL ->
    // exception_entry -> kernel_trap -> handle_syscall.
    sys_putc('>');
    sys_putc(' ');
    console_flush();

    for (;;) {
        // loop idle: todo el trabajo real lo hacen los handlers de IRQ
        // (eco de teclado, spinner del timer) en trap.c
    }
}
