// trap.c — dispatch de excepciones: interrupciones (timer/teclado),
// syscalls, y panic genérico para todo lo demás.
#include "gpu.h"
#include "keyboard.h"
#include "kernel.h"

static volatile u32 ticks = 0;

static void handle_syscall(TrapFrame *tf) {
    switch (tf->v0) {
        case SYS_PUTC:
            console_putc((char)tf->a0);
            console_flush();
            break;
        case SYS_GETC:
            // No bloqueante: sin scheduler todavía no hay a quién más
            // correr mientras se espera, así que devolvemos -1 si no hay
            // nada. El eco de teclado normal lo maneja keyboard_irq().
            if (kbd_has_event()) {
                u32 ev = kbd_read_event();
                tf->v0 = (ev & KBD_EVT_KIND_TEXT) ? (ev & 0xFF) : (u32)-1;
            } else {
                tf->v0 = (u32)-1;
            }
            break;
        case SYS_EXIT:
            kpanic(tf, "SYS_EXIT: sin scheduler todavia, no hay a donde volver");
            break;
        default:
            console_puts("[trap] syscall desconocida: ");
            console_put_uint(tf->v0);
            console_putc('\n');
            console_flush();
            break;
    }
    tf->epc += 4; // no reejecutar el syscall al volver
}

void timer_tick(void) {
    ticks++;
    cop0_write_compare(cop0_read_count() + TICK_PERIOD); // rearma, si no dispara una sola vez
    if (ticks % 20 == 0) {
        static const char spinner[4] = {'|', '/', '-', '\\'};
        // celda fija arriba a la derecha, no mueve el cursor de la consola
        gpu_text_putc(39, 0, spinner[(ticks / 20) & 3], 10, 0);
        console_flush();
    }
}

void keyboard_irq(void) {
    while (kbd_has_event()) {
        u32 ev = kbd_read_event();
        if (ev & KBD_EVT_KIND_TEXT) {
            console_putc((char)(ev & 0xFF));
        }
        // los eventos de tecla cruda (flechas, etc.) los ignora la consola
    }
    console_flush();
}

static void handle_interrupt(TrapFrame *tf) {
    if (tf->cause & CAUSE_IP7) {
        timer_tick();
    }
    if (tf->cause & CAUSE_IP2) {
        keyboard_irq();
    }
}

void kernel_trap(TrapFrame *tf) {
    u32 exc_code = (tf->cause & CAUSE_EXCCODE_MASK) >> CAUSE_EXCCODE_SHIFT;

    switch (exc_code) {
        case EXC_INT:
            handle_interrupt(tf);
            break;
        case EXC_SYS:
            handle_syscall(tf);
            break;
        default:
            kpanic(tf, "excepcion no manejada");
            break;
    }
}
