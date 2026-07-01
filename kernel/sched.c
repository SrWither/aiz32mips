// sched.c — scheduler round-robin. El proceso 0 es el propio kernel/shell
// (nunca se destruye: siempre hay alguien a quien volver). Los procesos de
// usuario van en los slots 1..MAX_PROCS-1, cada uno con su propio ASID/
// entrada de TLB (ver mm.c::mm_map_user) fijo desde que se crea.
#include "kernel.h"

#define MAX_PROCS 4

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED, // esperando un semáforo (sem.c); fuera de la rotación
} proc_state_t;

typedef struct {
    TrapFrame ctx;
    proc_state_t state;
} Process;

static Process proc_table[MAX_PROCS];
static int current_pid;

// El ASID del proceso elegido: exception_entry (boot.S) lo escribe en
// EntryHi recién en su última instrucción antes del eret, no acá. Si se
// escribiera desde C, todo el desenrolle de esta misma llamada (los
// "jr $ra" de sched_switch_to/sched_tick/.../kernel_trap) seguiría leyendo
// SU PROPIO stack — que vive en kuseg, en la página del proceso VIEJO —
// pero ya traducido contra la página del proceso NUEVO, corrompiendo el
// retorno. Por eso esto es solo una posta: un global en kseg1 (sin TLB de
// por medio) que boot.S lee al final, cuando el C ya no necesita más el
// stack de kuseg.
u32 g_next_asid;

// Copia manual: un TrapFrame asignado con "=" lo baja a memcpy, que no
// existe en este build sin libc (-nostdlib).
static void tf_copy(TrapFrame *dst, const TrapFrame *src) {
    u32 *d = (u32 *)dst;
    const u32 *s = (const u32 *)src;
    for (u32 i = 0; i < sizeof(*dst) / sizeof(u32); i++) {
        d[i] = s[i];
    }
}

void sched_init(void) {
    proc_table[0].state = PROC_RUNNING;
    current_pid = 0;
    g_next_asid = 0;
}

int sched_current_pid(void) {
    return current_pid;
}

// Ronda a partir de current_pid+1; si nadie más está listo, se queda con
// el mismo (el slot 0 siempre está RUNNING o READY, nunca UNUSED).
static int sched_pick_next(void) {
    for (int i = 1; i <= MAX_PROCS; i++) {
        int cand = (current_pid + i) % MAX_PROCS;
        if (proc_table[cand].state == PROC_READY) {
            return cand;
        }
    }
    return current_pid;
}

static void sched_switch_to(TrapFrame *tf, int next) {
    current_pid = next;
    proc_table[next].state = PROC_RUNNING;
    tf_copy(tf, &proc_table[next].ctx);
    g_next_asid = (u32)next; // boot.S lo escribe en EntryHi al final
}

// Guarda el contexto actual con `leave_state` (READY si lo interrumpió el
// timer, BLOCKED si se durmió en un semáforo) y pasa a correr el próximo
// listo. Comparten esto sched_tick y sched_block_current: solo difieren en
// qué estado le queda al que se va.
static void sched_save_and_switch(TrapFrame *tf, proc_state_t leave_state) {
    tf_copy(&proc_table[current_pid].ctx, tf);
    proc_table[current_pid].state = leave_state;
    sched_switch_to(tf, sched_pick_next());
}

void sched_tick(TrapFrame *tf) {
    sched_save_and_switch(tf, PROC_READY);
}

// La usa sem_wait (sem.c) cuando el proceso actual tiene que dormirse:
// a diferencia de sched_tick, quien llama ya dejó tf->epc apuntando
// después del syscall (no hay que reejecutarlo cuando lo despierten).
void sched_block_current(TrapFrame *tf) {
    sched_save_and_switch(tf, PROC_BLOCKED);
}

// La usa sem_signal (sem.c): solo cambia el estado, no fuerza ningún
// cambio de contexto — al que despierta lo agarra el scheduler en su
// próximo turno de round-robin.
void sched_wake(int pid) {
    if (proc_table[pid].state == PROC_BLOCKED) {
        proc_table[pid].state = PROC_READY;
    }
}

void sched_exit_current(TrapFrame *tf) {
    // Sin guardar ctx: este proceso no vuelve, no tiene sentido.
    proc_table[current_pid].state = PROC_UNUSED;
    sched_switch_to(tf, sched_pick_next());
}

int sched_spawn(const u8 *img, u32 img_len, u32 arg0) {
    int slot = -1;
    for (int i = 1; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    u32 prog_phys = pmm_alloc_page();
    u32 stack_phys = pmm_alloc_page();
    pmm_write_page(prog_phys, img, img_len);
    pmm_write_page(stack_phys, 0, 0);
    // A diferencia de sched_switch_to, esto SÍ puede escribir EntryHi
    // directo: spawn siempre corre en contexto del shell (proceso 0), cuyo
    // propio stack vive en kseg1 (no traducido), así que no hay ventana de
    // desenrolle afectada por el ASID que quede puesto.
    mm_map_user((u32)slot, prog_phys, stack_phys);

    TrapFrame *ctx = &proc_table[slot].ctx;
    u32 *words = (u32 *)ctx;
    for (u32 i = 0; i < sizeof(*ctx) / sizeof(u32); i++) {
        words[i] = 0;
    }
    ctx->a0 = arg0; // "rol": la app lo lee declarando void _start(int role)
    ctx->sp = 0x00402000u; // tope del stack (página impar), crece hacia abajo
    ctx->epc = 0x00400000u;
    // EXL=1 acá también: el epílogo de exception_entry hace mtc0 del status
    // y DESPUÉS sigue buscando instrucciones (lw/eret) en kseg1 — si
    // KSU=usuario ya estuviera activo sin EXL enmascarándolo, ese fetch
    // violaría el segmento (mismo problema que enter_user_mode antes de
    // borrarlo). eret limpia EXL solo, al ejecutarse.
    ctx->status = STATUS_BEV | STATUS_IE | STATUS_IM2 | STATUS_IM7 | STATUS_KSU_USER | STATUS_EXL;

    proc_table[slot].state = PROC_READY;
    return slot;
}
