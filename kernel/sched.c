// sched.c — scheduler round-robin. El proceso 0 es el propio kernel/shell
// (nunca se destruye: siempre hay alguien a quien volver). Los procesos de
// usuario van en los slots 1..MAX_PROCS-1, cada uno con su propio ASID/
// entrada de TLB (ver mm.c::mm_map_user) fijo desde que se crea.
#include "kernel.h"

// MAX_PROCS: en kernel.h (mm.c también lo necesita para separar los 2
// índices de TLB de cada proceso, ver mm_map_user).

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED, // esperando un semáforo (sem.c); fuera de la rotación
} proc_state_t;

typedef struct {
    TrapFrame ctx;
    proc_state_t state;
    // Las 2 páginas wired del proceso (texto+stack, ver mm_map_user):
    // sched_spawn las calcula pero antes las tiraba después de mapearlas —
    // sched_fork necesita poder leerlas de vuelta para saber qué copiarle
    // al hijo. El heap NO vive acá: es de mm.c (mm_fork_heap), tiene su
    // propia tabla de páginas por ASID.
    u32 prog_phys, stack_phys;
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
    // Si current_pid ya quedó UNUSED (sched_kill_all_user lo mató desde
    // keyboard_irq mientras JUSTO era el proceso corriendo, ver Ctrl+C en
    // shell.c) no lo resucitamos a READY acá: sin este chequeo, el
    // siguiente tick de timer revivía un proceso ya matado — por eso
    // "correr cube3d de nuevo" terminaba con dos instancias vivas.
    if (proc_table[current_pid].state != PROC_UNUSED) {
        proc_table[current_pid].state = leave_state;
    }
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

// Ctrl+C desde el shell (trap.c::keyboard_irq): mata todo lo que no sea el
// slot 0 (shell/kernel), sin importar su estado (corriendo, listo o
// bloqueado en un semáforo — un pingpong colgado en sys_sem_wait se saca
// así). No hay "proceso en foreground": es un botón de pánico simple, no
// selectivo. El slot que estuviera activo en este mismo instante (si el
// tick del teclado interrumpió a un proceso de usuario en vez de al
// shell) corre una última vez hasta el próximo tick del timer, que ya lo
// va a encontrar UNUSED y lo va a sacar de la rotación — no hace falta
// forzar un context switch acá mismo.
void sched_kill_all_user(void) {
    for (int i = 1; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED) {
            fs_close_all_owned_by(i); // si tenía un archivo abierto para escribir, lo vuelca antes de perderlo
        }
        proc_table[i].state = PROC_UNUSED;
    }
}

void sched_exit_current(TrapFrame *tf) {
    fs_close_all_owned_by(current_pid);
    // Sin guardar ctx: este proceso no vuelve, no tiene sentido.
    proc_table[current_pid].state = PROC_UNUSED;
    sched_switch_to(tf, sched_pick_next());
}

// ─────────────────────── carga de ELF32/MIPS desde disco ──────────────────
// Parser mínimo, hermano en espíritu del de aiz32mips_core::elf (que corre
// del lado host para bootear el propio kernel.elf): acá no hay Vec ni std,
// así que un tope fijo de PT_LOAD alcanza y sobra para binarios que caben
// en una sola página física (multi-página queda para cuando haya TLB
// refill de verdad).
#define ELF_MAX_SEGS 4
#define ELF_BUF_SIZE 8192

#define ELF_PT_LOAD 1u
#define ELF_EM_MIPS 8u

typedef struct {
    u32 vaddr;
    u32 offset;
    u32 filesz;
    u32 memsz;
} ElfSeg;

static u16 elf_rd16(const u8 *b, u32 off) {
    return (u16)(b[off] | (b[off + 1] << 8));
}

static u32 elf_rd32(const u8 *b, u32 off) {
    return (u32)b[off] | ((u32)b[off + 1] << 8) | ((u32)b[off + 2] << 16) | ((u32)b[off + 3] << 24);
}

// 0 ok, -1 si no es un ELF32 LE MIPS válido o tiene más PT_LOAD de los que
// entran en segs[].
static int elf_parse(const u8 *data, u32 len, u32 *entry, ElfSeg *segs, int *nsegs) {
    if (len < 52 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
        return -1;
    }
    if (data[4] != 1 || data[5] != 1) { // ELFCLASS32, ELFDATA2LSB
        return -1;
    }
    if (elf_rd16(data, 18) != ELF_EM_MIPS) {
        return -1;
    }
    *entry = elf_rd32(data, 24);
    u32 phoff = elf_rd32(data, 28);
    u32 phentsize = elf_rd16(data, 42);
    u32 phnum = elf_rd16(data, 44);

    *nsegs = 0;
    for (u32 i = 0; i < phnum; i++) {
        u32 ph = phoff + i * phentsize;
        if (ph + 32 > len) {
            return -1;
        }
        if (elf_rd32(data, ph) != ELF_PT_LOAD) {
            continue;
        }
        if (*nsegs >= ELF_MAX_SEGS) {
            return -1;
        }
        ElfSeg *s = &segs[*nsegs];
        s->offset = elf_rd32(data, ph + 4);
        s->vaddr = elf_rd32(data, ph + 8);
        s->filesz = elf_rd32(data, ph + 16);
        s->memsz = elf_rd32(data, ph + 20);
        if (s->offset + s->filesz > len) {
            return -1;
        }
        (*nsegs)++;
    }
    return 0;
}

int sched_spawn(const char *path, u32 arg0) {
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

    // static: sched_spawn siempre corre en contexto del shell (nunca
    // reentra), así que un solo buffer de scratch alcanza — evita 8KB en
    // un stack de kernel que ya es chico (ver el comentario equivalente en
    // fs.c).
    static u8 elf_buf[ELF_BUF_SIZE];
    int n = fs_read(path, elf_buf, sizeof(elf_buf));
    if (n <= 0) {
        return -1;
    }

    u32 entry;
    ElfSeg segs[ELF_MAX_SEGS];
    int nsegs;
    if (elf_parse(elf_buf, (u32)n, &entry, segs, &nsegs) < 0) {
        return -1;
    }

    u32 prog_phys = pmm_alloc_page();
    u32 stack_phys = pmm_alloc_page();
    pmm_write_page(prog_phys, 0, 0); // huecos entre segmentos y bss: en 0
    for (int i = 0; i < nsegs; i++) {
        // Todo PT_LOAD tiene que caer dentro de la única página de
        // texto+datos del proceso (el stack es página aparte, el heap ni
        // eso — ver mm.c): un ELF armado a mano o corrupto que pise esto
        // se rechaza acá en vez de corromper la página de al lado.
        if (segs[i].vaddr < USER_VADDR) {
            return -1;
        }
        u32 seg_off = segs[i].vaddr - USER_VADDR;
        if (seg_off + segs[i].memsz > 0x1000u) {
            return -1;
        }
        pmm_write_page_at(prog_phys, seg_off, elf_buf + segs[i].offset, segs[i].filesz);
    }
    pmm_write_page(stack_phys, 0, 0);
    // mm_reset_heap ANTES de mapear: si este slot lo tuvo otro proceso
    // antes, hay que limpiar su tabla de páginas de heap y tirar abajo
    // cualquier entrada de TLB que le hubiera quedado viva con este mismo
    // ASID (ver el comentario de mm_reset_heap) — si no, el proceso nuevo
    // podría heredar heap ajeno.
    mm_reset_heap((u32)slot);
    // A diferencia de sched_switch_to, esto SÍ puede escribir EntryHi
    // directo: spawn siempre corre en contexto del shell (proceso 0), cuyo
    // propio stack vive en kseg1 (no traducido), así que no hay ventana de
    // desenrolle afectada por el ASID que quede puesto.
    mm_map_user((u32)slot, prog_phys, stack_phys);
    proc_table[slot].prog_phys = prog_phys;
    proc_table[slot].stack_phys = stack_phys;

    TrapFrame *ctx = &proc_table[slot].ctx;
    u32 *words = (u32 *)ctx;
    for (u32 i = 0; i < sizeof(*ctx) / sizeof(u32); i++) {
        words[i] = 0;
    }
    ctx->a0 = arg0; // "rol": la app lo lee declarando void _start(int role)
    ctx->sp = USER_STACK_VADDR + 0x1000u; // tope del stack, crece hacia abajo
    ctx->epc = entry;
    // EXL=1 acá también: el epílogo de exception_entry hace mtc0 del status
    // y DESPUÉS sigue buscando instrucciones (lw/eret) en kseg1 — si
    // KSU=usuario ya estuviera activo sin EXL enmascarándolo, ese fetch
    // violaría el segmento (mismo problema que enter_user_mode antes de
    // borrarlo). eret limpia EXL solo, al ejecutarse.
    ctx->status = STATUS_BEV | STATUS_IE | STATUS_IM2 | STATUS_IM7 | STATUS_KSU_USER | STATUS_EXL;

    proc_table[slot].state = PROC_READY;
    return slot;
}

// fork(): duplica al proceso actual (4 páginas físicas copiadas enteras,
// sin copy-on-write — ver pmm_copy_page) en un slot nuevo, con el mismo
// contexto de registros que tenía en el momento del syscall. El único
// dato que difiere entre padre e hijo es v0 (valor de retorno de fork):
// el padre lo pisa afuera, en trap.c, con el pid que devolvemos acá; al
// hijo se lo dejamos en 0 directamente en su copia del contexto.
int sched_fork(TrapFrame *tf) {
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

    Process *parent = &proc_table[current_pid];
    u32 prog_phys = pmm_alloc_page();
    u32 stack_phys = pmm_alloc_page();
    pmm_copy_page(prog_phys, parent->prog_phys);
    pmm_copy_page(stack_phys, parent->stack_phys);
    // mm_reset_heap + mm_fork_heap: mismo motivo que en sched_spawn (este
    // slot puede venir de un proceso anterior), pero acá además hay que
    // copiarle al hijo el heap que el padre ya hubiera tocado.
    mm_reset_heap((u32)slot);
    mm_fork_heap((u32)slot, (u32)current_pid);
    // Mismo comentario que en sched_spawn: fork siempre lo llama un
    // proceso corriendo (nunca el shell "desde afuera"), pero el shell
    // TAMPOCO usa TLB propio (kseg1), así que esto sigue siendo seguro.
    mm_map_user((u32)slot, prog_phys, stack_phys);

    Process *child = &proc_table[slot];
    child->prog_phys = prog_phys;
    child->stack_phys = stack_phys;
    tf_copy(&child->ctx, tf); // mismos registros que el padre en este instante...
    child->ctx.v0 = 0;        // ...salvo el retorno de fork(): 0 en el hijo
    child->ctx.epc = tf->epc + 4; // no reejecutar el syscall (igual que el padre, ver trap.c)
    child->state = PROC_READY;

    return slot; // el padre recibe el pid del hijo (lo pisa trap.c en tf->v0)
}
