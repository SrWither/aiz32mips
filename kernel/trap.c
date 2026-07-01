// trap.c — dispatch de excepciones: interrupciones (timer/teclado),
// syscalls, y panic genérico para todo lo demás.
#include "gpu.h"
#include "keyboard.h"
#include "kernel.h"

static volatile u32 ticks = 0;

// Reescribe el frame para que el eret de exception_entry aterrice de vuelta
// en el shell del kernel, como si enter_user_mode() (boot.S) hubiera hecho
// un return normal. La usan tanto SYS_EXIT (salida limpia) como el
// aislamiento de fallos de kernel_trap (proceso que se cayó solo).
static void return_to_kernel(TrapFrame *tf) {
    tf->epc = g_kexit_pc;
    tf->sp = g_kexit_sp;
    // callee-saved: lo que el proceso de usuario haya dejado en estos
    // registros (p.ej. $16 como puntero de su propio loop) no puede quedar
    // pisando el contexto del kernel al que volvemos.
    tf->s0 = g_kexit_ctx[0];
    tf->s1 = g_kexit_ctx[1];
    tf->s2 = g_kexit_ctx[2];
    tf->s3 = g_kexit_ctx[3];
    tf->s4 = g_kexit_ctx[4];
    tf->s5 = g_kexit_ctx[5];
    tf->s6 = g_kexit_ctx[6];
    tf->s7 = g_kexit_ctx[7];
    tf->gp = g_kexit_ctx[8];
    tf->fp = g_kexit_ctx[9];
    tf->status &= ~STATUS_KSU_MASK;
}

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
            return_to_kernel(tf);
            return; // sin el epc+=4 de abajo: ya apunta donde corresponde
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
            shell_input((char)(ev & 0xFF));
        } else if (ev & KBD_EVT_PRESSED) {
            // de las teclas crudas (flechas, etc.) sólo nos importan
            // backspace y enter, que no llegan como texto traducido.
            u32 keycode = ev & 0xFF;
            if (keycode == 8 || keycode == 13) {
                shell_input((char)keycode);
            }
        }
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

// Excepción no manejada (TLBL/TLBS/AdE/RI/Ov/...) con el proceso corriendo
// en modo usuario: no es un bug del kernel, es el programa de usuario el
// que se cayó. Lo matamos a él solo (mismo mecanismo que SYS_EXIT) en vez
// de tirar todo el kernel abajo con kpanic.
static void kill_user_process(TrapFrame *tf, u32 exc_code) {
    console_putc('\n');
    console_puts("[user] proceso terminado por excepcion: ");
    console_puts(exc_name(exc_code));
    console_putc('\n');
    console_flush();
    return_to_kernel(tf);
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
            if ((tf->status & STATUS_KSU_MASK) == STATUS_KSU_USER) {
                kill_user_process(tf, exc_code);
            } else {
                kpanic(tf, "excepcion no manejada");
            }
            break;
    }
}
