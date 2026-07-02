// trap.c — dispatch de excepciones: interrupciones (timer/teclado),
// syscalls, y panic genérico para todo lo demás.
#include "audio.h"
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

// La usa sched.c (sched_signal_fg) cuando la acción default de una señal
// termina un proceso puntual: a diferencia del viejo gpu_force_release
// (que la devolvía sí o sí, pensado para cuando Ctrl+C mataba TODOS los
// procesos), esta sólo libera si el que se mató era justo el dueño.
void gpu_release_if_owner(int pid) {
    if (pid == gfx_owner_pid) {
        gfx_release();
    }
}

// Mismo patrón que gfx_owner_pid, para el último proceso que sometió
// audio (ver SYS_AUDIO_SUBMIT), pero OJO: a diferencia de la GPU, esto
// sólo se usa en salidas ANORMALES (Ctrl+C, excepción) — un SYS_EXIT
// normal NO llama a esto (ver ese case), porque el audio ya sometido es
// trabajo terminado esperando reproducirse ("someto un sonido y salgo" es
// el patrón normal, no un caso raro a limpiar).
static int audio_owner_pid = -1;

void audio_release_if_owner(int pid) {
    if (pid == audio_owner_pid) {
        REG_AUDIO_CTRL = 0; // corta y vacía la cola de reproducción (ver AudioMmio::write8)
        audio_owner_pid = -1;
    }
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
            // A diferencia de la GPU (un modo activo que no tiene sentido
            // seguir "renderizando" sin nadie mandando frames), el audio ya
            // sometido es trabajo terminado esperando reproducirse: cortarlo
            // acá rompería el patrón normal de "someto un sonido y salgo"
            // (ver user/audiotst.c) — sólo se corta en una salida ANORMAL
            // (Ctrl+C, excepción; ver sched_signal_fg/kill_user_process).
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
        case SYS_FORK:
            // El hijo ya queda armado con su propio v0=0 dentro de
            // sched_fork; acá sólo se pisa el del padre (este mismo tf)
            // con el pid del hijo, o -1 si no había slot.
            tf->v0 = (u32)sched_fork(tf);
            break;
        case SYS_WAIT:
            if (sched_wait_for(tf, (int)tf->a0)) {
                return; // se bloqueó: tf ya es de otro proceso
            }
            break; // no bloqueó (tf->v0 ya en -1, ver sched_wait_for): epc+=4 de abajo
        case SYS_OPEN:
            // tf->a0 es un puntero kuseg del proceso (path): se puede leer
            // directo, mismo motivo que en SYS_GPU_SUBMIT (ver abi.h).
            tf->v0 = (u32)fs_open((const char *)tf->a0, (int)tf->a1, sched_current_pid());
            break;
        case SYS_READ:
            tf->v0 = (u32)fs_fd_read((int)tf->a0, (u8 *)tf->a1, tf->a2);
            break;
        case SYS_WRITE:
            tf->v0 = (u32)fs_fd_write((int)tf->a0, (const u8 *)tf->a1, tf->a2);
            break;
        case SYS_CLOSE:
            tf->v0 = (u32)fs_fd_close((int)tf->a0);
            break;
        case SYS_LSEEK:
            tf->v0 = (u32)fs_fd_seek((int)tf->a0, (i32)tf->a1, (int)tf->a2);
            break;
        case SYS_AUDIO_SUBMIT: {
            // tf->a0 es un puntero kuseg del proceso (mismo motivo que
            // SYS_GPU_SUBMIT/SYS_OPEN: el ASID/TLB activo en este preciso
            // momento siguen siendo los suyos). Se trocea en tandas de
            // AUDIO_BUF_SAMPLES porque el staging del device sólo tiene
            // lugar para eso de una — igual que gpu_submit con VRAM_CMDBUF.
            const short *src = (const short *)tf->a0;
            u32 n = tf->a1;
            audio_owner_pid = sched_current_pid();
            REG_AUDIO_CTRL = 1; // por si un audio_release_if_owner anterior lo había apagado
            u32 done = 0;
            while (done < n) {
                u32 chunk = n - done;
                if (chunk > AUDIO_BUF_SAMPLES) {
                    chunk = AUDIO_BUF_SAMPLES;
                }
                for (u32 i = 0; i < chunk; i++) {
                    AUDIO_BUF[i] = src[done + i];
                }
                REG_AUDIO_LEN = chunk;
                REG_AUDIO_KICK = 1;
                done += chunk;
            }
            break;
        }
        case SYS_AUDIO_STATUS:
            tf->v0 = REG_AUDIO_STATUS;
            break;
        case SYS_SIGNAL:
            tf->v0 = sched_set_sig_handler(sched_current_pid(), (int)tf->a0, tf->a1, tf->a2);
            break;
        case SYS_SIGRETURN:
            sched_sigreturn(tf, sched_current_pid());
            return; // sin el epc+=4 de abajo: ya restaura el epc pre-señal tal cual (igual que SYS_EXIT)
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

void keyboard_irq(TrapFrame *tf) {
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
            } else if (keycode == KBD_KC_UP) {
                shell_history_up();
            } else if (keycode == KBD_KC_DOWN) {
                shell_history_down();
            } else if (keycode == 'c' && (ev & KBD_MOD_CTRL)) {
                // Ctrl+C: SDL no lo manda como TextInput (los combos con
                // Ctrl quedan fuera de esa traducción). Se entrega SIGINT
                // al proceso en foreground (sched_signal_fg necesita tf: si
                // ESE proceso es justo el que este mismo IRQ interrumpió,
                // hay que redirigir su estado en vivo, no el guardado en
                // proc_table). was_waiting se captura ANTES del kill: una
                // vez que sched_signal_fg mata al proceso (acción default),
                // shell_on_process_exit ya limpió fg_wait_pid — shell_ctrl_c
                // necesita saber si HABÍA algo en foreground, no si sigue
                // habiendo, para no imprimir el prompt dos veces.
                int was_waiting = shell_is_waiting_fg();
                int killed_pid = sched_signal_fg(tf, SIGINT);
                if (killed_pid > 0) {
                    gpu_release_if_owner(killed_pid);
                    audio_release_if_owner(killed_pid);
                }
                shell_ctrl_c(was_waiting);
            } else if (keycode == 'l' && (ev & KBD_MOD_CTRL)) {
                // Ctrl+L: mismo motivo que Ctrl+C arriba (SDL no lo manda
                // como TextInput) — acá no hace falta tocar el scheduler,
                // es puramente visual (ver el case c==12 en shell.c).
                shell_input((char)12);
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
        keyboard_irq(tf);
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
    audio_release_if_owner(sched_current_pid());
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
            return;
        case EXC_SYS:
            handle_syscall(tf);
            return;
        case EXC_TLBL:
        case EXC_TLBS:
            // TLB refill/inválida: antes de tratarla como un bug del
            // proceso, fijarse si cae en su heap (con demanda de páginas,
            // ver mm_handle_page_fault) — si es así, se arma la entrada
            // de TLB que faltaba y volvemos sin tocar epc, para que la
            // instrucción que voló se reintente sola (a diferencia de una
            // syscall, acá no hay "+4" que valga).
            if ((tf->status & STATUS_KSU_MASK) == STATUS_KSU_USER && mm_handle_page_fault(tf->badvaddr)) {
                return;
            }
            break; // no era del heap (o pasó en modo kernel): es una excepción de verdad
        default:
            break;
    }

    if ((tf->status & STATUS_KSU_MASK) == STATUS_KSU_USER) {
        kill_user_process(tf, exc_code);
    } else {
        kpanic(tf, "excepcion no manejada");
    }
}
