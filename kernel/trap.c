// trap.c — dispatch de excepciones: interrupciones (timer/teclado),
// syscalls, y panic genérico para todo lo demás.
#include "gpu.h"
#include "keyboard.h"
#include "kernel.h"

static volatile u32 ticks = 0;

// GPU ownership: mientras un proceso está "dueño" de la pantalla (desde su
// sys_gpu_init hasta que sale o lo matan), console_flush() no toca la GPU
// — si lo hiciera, el spinner del timer (más abajo) o el eco de teclado
// pisarían el modo texto/doble-buffer del proceso en cada tick, así de
// hecho desaparecía el cubo de cube3d.c antes de este fix, aun con
// VRAM_USER_CMDBUF separado del buffer del kernel.
static int gfx_owner_pid = -1;

int gpu_owned_by_user(void) {
    return gfx_owner_pid >= 0;
}

// Vuelve al modo consola de siempre (single-buffer 320x200, sin Z, texto
// habilitado) y limpia lo que hubiera quedado en pantalla del proceso
// gráfico.
static void gfx_release(void) {
    gfx_owner_pid = -1;
    gpu_init(320, 200, 0, 0);
    REG_TEXT_ENABLE = 1;
    console_clear();
}

void gpu_force_release(void) {
    gfx_release();
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
            if (sched_current_pid() == gfx_owner_pid) {
                gfx_release();
            }
            sched_exit_current(tf);
            return; // sin el epc+=4 de abajo: ya apunta donde corresponde
        case SYS_SEM_WAIT:
            if (sem_wait(tf, (int)tf->a0)) {
                return; // se durmió: tf ya es de otro proceso
            }
            break; // no bloqueó: sigue normal, epc+=4 de abajo
        case SYS_SEM_SIGNAL:
            sem_signal((int)tf->a0);
            break;
        case SYS_GPU_INIT:
            // A partir de acá el proceso es dueño de la pantalla (ver
            // gfx_owner_pid arriba): apaga el texto, que a partir de ahora
            // sólo lo pisaría.
            gfx_owner_pid = sched_current_pid();
            REG_TEXT_ENABLE = 0;
            gpu_init((u16)tf->a0, (u16)tf->a1, (int)tf->a2, (int)tf->a3);
            break;
        case SYS_GPU_SUBMIT: {
            // tf->a0 es un puntero kuseg del proceso que hizo el syscall:
            // se puede desreferenciar tal cual porque el ASID/TLB activo
            // en este preciso momento siguen siendo los suyos (nada los
            // cambia entre el syscall y este handler, ver abi.h).
            const u32 *src = (const u32 *)tf->a0;
            u32 n = tf->a1;
            if (n > 4096) { // tope defensivo: no pisar más allá de VRAM_USER_CMDBUF
                n = 4096;
            }
            // VRAM_USER_CMDBUF, no VRAM_CMDBUF: esa la sigue usando el
            // kernel para sus propios flips de consola (console_flush) —
            // compartirla pisaba la display list del proceso a mitad de
            // frame (ver el comentario en gpu.h).
            volatile u32 *dst = (volatile u32 *)(VRAM_BASE + VRAM_USER_CMDBUF);
            for (u32 i = 0; i < n; i++) {
                dst[i] = src[i];
            }
            if (n > 0) {
                REG_CMD_ADDR = VRAM_USER_CMDBUF;
                REG_CMD_LEN = n;
                REG_CMD_KICK = 1;
            }
            break;
        }
        case SYS_GPU_STATUS:
            tf->v0 = REG_STATUS;
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

void timer_tick(TrapFrame *tf) {
    ticks++;
    cop0_write_compare(cop0_read_count() + TICK_PERIOD); // rearma, si no dispara una sola vez
    if (ticks % 20 == 0) {
        static const char spinner[4] = {'|', '/', '-', '\\'};
        // celda fija arriba a la derecha, no mueve el cursor de la consola
        gpu_text_putc(39, 0, spinner[(ticks / 20) & 3], 10, 0);
        console_flush();
    }
    sched_tick(tf); // preemption: round-robin en cada tick
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
            } else if (keycode == 'c' && (ev & KBD_MOD_CTRL)) {
                // Ctrl+C: SDL no lo manda como TextInput (los combos con
                // Ctrl quedan fuera de esa traducción), así que se arma acá
                // el byte ASCII de ETX (3) que shell_input sabe interpretar.
                shell_input((char)3);
            }
        }
    }
    console_flush();
}

static void handle_interrupt(TrapFrame *tf) {
    if (tf->cause & CAUSE_IP7) {
        timer_tick(tf);
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
    // Antes de imprimir nada: si el que se cayó era el dueño de la
    // pantalla, console_flush() de abajo no hace nada hasta que se libere
    // (ver gfx_release) — sin esto el mensaje de error queda escrito en
    // VRAM_TEXT pero invisible.
    if (sched_current_pid() == gfx_owner_pid) {
        gfx_release();
    }
    console_putc('\n');
    console_puts("[user] proceso terminado por excepcion: ");
    console_puts(exc_name(exc_code));
    console_putc('\n');
    console_flush();
    sched_exit_current(tf);
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
