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

// ELF_MAX_SEGS: definido más abajo junto al resto del parser de ELF, pero
// la struct hace falta acá arriba porque Process guarda una copia por
// proceso (para poder re-leer el texto del ELF a demanda, ver
// sched_load_text_page). Se declara antes de Process, en vez de mover
// Process más abajo, para no reordenar el resto del archivo.
#define ELF_MAX_SEGS 4

typedef struct {
    u32 vaddr;
    u32 offset;
    u32 filesz;
    u32 memsz;
} ElfSeg;

typedef struct {
    TrapFrame ctx;
    proc_state_t state;
    // Ya no hay una página física fija de texto (prog_phys): texto/datos/
    // bss son demand-paged igual que el heap (ver mm.c::text_page_table),
    // re-leídos del ELF en disco página por página (sched_load_text_page).
    // Lo que sched_spawn necesita conservar por proceso para eso es la
    // ruta del ELF y sus segmentos PT_LOAD — sched_fork copia estos 3
    // campos tal cual del padre al hijo (mismo binario, mismos offsets).
    char elf_path[96];
    ElfSeg text_segs[ELF_MAX_SEGS];
    int n_text_segs;
    // El stack SÍ sigue siendo una página física fija y wired (ver
    // mm_map_user) — sched_fork necesita poder leerla de vuelta para
    // saber qué copiarle al hijo. El heap NO vive acá: es de mm.c
    // (mm_fork_heap), tiene su propia tabla de páginas por ASID.
    u32 stack_phys;
    // Señales (ver sched_signal_fg/sched_set_sig_handler/sched_sigreturn):
    // sig_handler[n] es SIG_DFL(0)/SIG_IGN(1)/una dirección de handler de
    // usuario para la señal n. sig_trampoline es la dirección (fija, la
    // misma para todas las señales de este proceso) a la que el handler
    // vuelve — la instala sys_signal, ver user/libc/signal.h. sig_in_handler
    // evita anidar: mientras está en 1, una nueva entrega de la misma señal
    // no hace nada (v1, sin cola de señales). sig_saved_ctx es el TrapFrame
    // completo de justo antes de saltar al handler, para que sys_sigreturn
    // lo restaure tal cual.
    u32 sig_handler[NSIG];
    u32 sig_trampoline;
    u32 sig_in_handler;
    TrapFrame sig_saved_ctx;
} Process;

static Process proc_table[MAX_PROCS];
static int current_pid;

// El último pid que arrancó sched_spawn: no hay job control de verdad (los
// procesos ya corren todos async por round-robin, ver el comentario de
// keyboard_irq en trap.c), así que "el proceso en foreground" para Ctrl+C
// es simplemente el último comando que tipeaste — igual que una shell
// simple sin `fg`/`bg`. 0 = ninguno (apunta al shell mismo, que no es un
// slot matable).
static int fg_pid;

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
    // Chequeo defensivo: si current_pid ya quedó UNUSED no lo resucitamos a
    // READY acá. sched_kill_pid/sched_exit_current fuerzan un context
    // switch inmediato cuando matan al proceso corriendo, así que hoy este
    // caso no debería darse — pero sin el chequeo, si algún camino futuro
    // marcara UNUSED sin cambiar de contexto, el siguiente tick revivía un
    // proceso ya muerto (así se manifestó originalmente: "correr cube3d de
    // nuevo" terminaba con dos instancias vivas).
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

// Termina un pid puntual (acción default de una señal sin handler
// instalado, ver sched_signal_fg) sin tocar a los demás procesos, sin
// importar su estado (corriendo, listo o bloqueado en un semáforo — un
// pingpong colgado en sys_sem_wait se saca así, igual que antes). Si el pid
// es el que está corriendo ahora mismo, fuerza el cambio de contexto con tf
// en vez de dejarlo correr una última vez hasta el próximo tick.
void sched_kill_pid(TrapFrame *tf, int pid) {
    if (proc_table[pid].state == PROC_UNUSED) {
        return;
    }
    fs_close_all_owned_by(pid); // si tenía un archivo abierto para escribir, lo vuelca antes de perderlo
    proc_table[pid].state = PROC_UNUSED;
    if (pid == current_pid) {
        sched_switch_to(tf, sched_pick_next());
    }
}

// Ctrl+C desde el shell (trap.c::keyboard_irq): a diferencia del viejo
// "matar todo lo que no sea el shell", esto entrega SIGINT solo al proceso
// en foreground (fg_pid). Si no instaló un handler con sys_signal, la
// acción default es la de siempre (sched_kill_pid); si instaló uno, se lo
// redirige ahí en vez de matarlo. Devuelve el pid que se mató por acción
// default (para que trap.c libere la GPU si era el dueño), o 0 si no pasó
// nada (sin foreground, señal ignorada, o redirigida a un handler).
int sched_signal_fg(TrapFrame *tf, int signum) {
    int pid = fg_pid;
    if (pid == 0 || proc_table[pid].state == PROC_UNUSED) {
        return 0;
    }
    u32 handler = proc_table[pid].sig_handler[signum];
    if (handler == (u32)SIG_IGN) {
        return 0;
    }
    if (handler == (u32)SIG_DFL) {
        sched_kill_pid(tf, pid);
        return pid;
    }
    if (proc_table[pid].sig_in_handler) {
        return 0; // ya está atendiendo esta misma señal: no se anida (v1, sin cola)
    }
    // Bloqueado en un semáforo: sacarlo de ahí para que el scheduler lo
    // corra y llegue a ejecutar el handler (si no, se queda esperando un
    // sem_signal que puede no llegar nunca mientras el handler espera).
    if (proc_table[pid].state == PROC_BLOCKED) {
        proc_table[pid].state = PROC_READY;
    }
    // Si pid es el que está corriendo ahora mismo, su estado en vivo es tf
    // (todavía no se volcó a proc_table[pid].ctx); si no, ya está guardado
    // ahí desde el último context switch.
    TrapFrame *target = (pid == current_pid) ? tf : &proc_table[pid].ctx;
    tf_copy(&proc_table[pid].sig_saved_ctx, target);
    proc_table[pid].sig_in_handler = 1;
    target->a0 = (u32)signum;
    target->epc = handler;
    target->ra = proc_table[pid].sig_trampoline;
    return 0;
}

// SYS_SIGNAL: instala `handler` (+ su trampolín de retorno) para `signum`
// en el proceso `pid`, devuelve el handler anterior (o SIG_ERR si signum
// está fuera de rango — sin este chequeo, un signum negativo o gigante
// escribiría fuera de sig_handler[NSIG]).
u32 sched_set_sig_handler(int pid, int signum, u32 handler, u32 trampoline) {
    if (signum <= 0 || signum >= NSIG) {
        return (u32)SIG_ERR;
    }
    u32 old = proc_table[pid].sig_handler[signum];
    proc_table[pid].sig_handler[signum] = handler;
    proc_table[pid].sig_trampoline = trampoline;
    return old;
}

// SYS_SIGRETURN: la llama _sig_trampoline (user/libc/signal.h) al volver de
// un handler. Restaura el TrapFrame completo de justo antes de la señal —
// tf queda con el epc original tal cual (el trap.c que la invoca no debe
// hacerle el +4 de post-syscall habitual, ver el "return" en su case).
void sched_sigreturn(TrapFrame *tf, int pid) {
    if (!proc_table[pid].sig_in_handler) {
        return; // no-op defensivo: nadie entregó una señal con handler pendiente
    }
    tf_copy(tf, &proc_table[pid].sig_saved_ctx);
    proc_table[pid].sig_in_handler = 0;
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
// así que un tope fijo de PT_LOAD alcanza y sobra (la cabecera del ELF —
// hasta ELF_BUF_SIZE bytes — es lo único que hace falta leer entera acá;
// el contenido de cada PT_LOAD se re-lee del disco a demanda, página por
// página, ver sched_load_text_page).
// ElfSeg está declarada arriba, junto a Process (sched_fork necesita
// copiar text_segs[] del padre al hijo).
#define ELF_BUF_SIZE 8192

#define ELF_PT_LOAD 1u
#define ELF_EM_MIPS 8u

// Copia acotada de string, sin depender de libc (mismo patrón que
// shell.c::str_copy / fs.c::fs_str_copy — cada archivo tiene el suyo en
// este proyecto, no hay una versión compartida).
static void str_copy(char *dst, const char *src, int maxlen) {
    int i = 0;
    for (; i < maxlen - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

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
        // Invariante básica del formato ELF, no una validación contra
        // `len`: `data` acá es solo la cabecera (hasta ELF_BUF_SIZE bytes,
        // ver sched_spawn), el contenido real de cada PT_LOAD se re-lee
        // del disco a demanda (sched_load_text_page), así que puede ser
        // mucho más grande que `len` sin que eso sea un error.
        if (s->filesz > s->memsz) {
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
    for (int i = 0; i < nsegs; i++) {
        // Todo PT_LOAD tiene que caer dentro de la región de texto/datos/
        // bss demand-paged del proceso (el stack vive en su propia región
        // aparte, el heap ni eso — ver mm.c/kernel.h): un ELF armado a
        // mano o corrupto que pise esto se rechaza acá, antes de guardar
        // nada, en vez de fallar más tarde a mitad de un page fault.
        if (segs[i].vaddr < USER_VADDR) {
            return -1;
        }
        u32 seg_off = segs[i].vaddr - USER_VADDR;
        if (seg_off + segs[i].memsz > USER_TEXT_SIZE) {
            return -1;
        }
    }

    // Ya no se copia el contenido del ELF a una página física acá: texto/
    // datos/bss son demand-paged (ver mm.c::text_page_get_or_alloc), así
    // que lo único que hace falta guardar por proceso es de dónde re-leer
    // cada página cuando se toque por primera vez (sched_load_text_page).
    u32 stack_phys = pmm_alloc_page();
    pmm_write_page(stack_phys, 0, 0);
    // mm_reset_process ANTES de mapear: si este slot lo tuvo otro proceso
    // antes, hay que limpiar sus tablas de páginas (texto+heap) y tirar
    // abajo cualquier entrada de TLB que le hubiera quedado viva con este
    // mismo ASID (ver el comentario de mm_reset_process) — si no, el
    // proceso nuevo podría heredar texto o heap ajenos.
    mm_reset_process((u32)slot);
    // A diferencia de sched_switch_to, esto SÍ puede escribir EntryHi
    // directo: spawn siempre corre en contexto del shell (proceso 0), cuyo
    // propio stack vive en kseg1 (no traducido), así que no hay ventana de
    // desenrolle afectada por el ASID que quede puesto.
    mm_map_user((u32)slot, stack_phys);
    proc_table[slot].stack_phys = stack_phys;
    str_copy(proc_table[slot].elf_path, path, (int)sizeof(proc_table[slot].elf_path));
    proc_table[slot].n_text_segs = nsegs;
    for (int i = 0; i < nsegs; i++) {
        // Campo a campo, no `=`: una struct asignada entera baja a un
        // memcpy que no existe en este build sin libc (mismo motivo que
        // tf_copy más arriba, ver su comentario).
        proc_table[slot].text_segs[i].vaddr = segs[i].vaddr;
        proc_table[slot].text_segs[i].offset = segs[i].offset;
        proc_table[slot].text_segs[i].filesz = segs[i].filesz;
        proc_table[slot].text_segs[i].memsz = segs[i].memsz;
    }

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

    // Señales a SIG_DFL: si este slot lo tuvo otro proceso antes, sin esto
    // el nuevo heredaría los handlers (y hasta el sig_in_handler) del que
    // ocupaba el slot previamente.
    for (int i = 0; i < NSIG; i++) {
        proc_table[slot].sig_handler[i] = (u32)SIG_DFL;
    }
    proc_table[slot].sig_trampoline = 0;
    proc_table[slot].sig_in_handler = 0;

    proc_table[slot].state = PROC_READY;
    // Este es el nuevo "foreground": Ctrl+C apunta al último comando que se
    // arrancó desde el shell (ver sched_signal_fg). fork() NO pisa esto —
    // un hijo creado por sys_fork es parte del mismo comando, no un nuevo
    // foreground propio.
    fg_pid = slot;
    return slot;
}

// La llama mm.c (mm_handle_page_fault → text_page_get_or_alloc) cuando el
// proceso `asid` toca por primera vez una página de texto/datos/bss:
// rellena `dst` (PAGE_SIZE bytes, puntero kseg0 de la página física recién
// asignada) con los bytes reales del ELF en disco, y deja el resto en
// cero — cubre tanto huecos entre segmentos como bss más allá de filesz.
void sched_load_text_page(u32 asid, u32 page_vaddr, u8 *dst) {
    Process *p = &proc_table[asid];
    for (u32 i = 0; i < 0x1000u; i++) {
        dst[i] = 0;
    }
    u32 page_off = page_vaddr - USER_VADDR;
    for (int i = 0; i < p->n_text_segs; i++) {
        ElfSeg *s = &p->text_segs[i];
        u32 seg_off = s->vaddr - USER_VADDR;
        u32 seg_file_end = seg_off + s->filesz;
        // Intersección entre [page_off, page_off+PAGE_SIZE) (esta página)
        // y [seg_off, seg_file_end) (la parte de ESTE segmento que tiene
        // bytes reales en el archivo — más allá de eso es bss, ya en cero
        // por el memset de arriba).
        u32 lo = (page_off > seg_off) ? page_off : seg_off;
        u32 hi = (page_off + 0x1000u < seg_file_end) ? page_off + 0x1000u : seg_file_end;
        if (lo < hi) {
            u32 file_off = s->offset + (lo - seg_off);
            fs_read_at(p->elf_path, file_off, dst + (lo - page_off), hi - lo);
        }
    }
}

// fork(): duplica al proceso actual (páginas físicas ya tocadas de texto/
// heap, más el stack entero, copiadas sin copy-on-write — ver
// pmm_copy_page/mm_fork_text/mm_fork_heap) en un slot nuevo, con el mismo
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
    u32 stack_phys = pmm_alloc_page();
    pmm_copy_page(stack_phys, parent->stack_phys);
    // mm_reset_process + mm_fork_text/mm_fork_heap: mismo motivo que en
    // sched_spawn (este slot puede venir de un proceso anterior), pero acá
    // además hay que copiarle al hijo el texto y el heap que el padre ya
    // hubiera tocado.
    mm_reset_process((u32)slot);
    mm_fork_text((u32)slot, (u32)current_pid);
    mm_fork_heap((u32)slot, (u32)current_pid);
    // Mismo comentario que en sched_spawn: fork siempre lo llama un
    // proceso corriendo (nunca el shell "desde afuera"), pero el shell
    // TAMPOCO usa TLB propio (kseg1), así que esto sigue siendo seguro.
    mm_map_user((u32)slot, stack_phys);

    Process *child = &proc_table[slot];
    child->stack_phys = stack_phys;
    str_copy(child->elf_path, parent->elf_path, (int)sizeof(child->elf_path));
    child->n_text_segs = parent->n_text_segs;
    for (int i = 0; i < parent->n_text_segs; i++) {
        // Campo a campo, no `=`: mismo motivo que en sched_spawn (sin
        // memcpy en este build).
        child->text_segs[i].vaddr = parent->text_segs[i].vaddr;
        child->text_segs[i].offset = parent->text_segs[i].offset;
        child->text_segs[i].filesz = parent->text_segs[i].filesz;
        child->text_segs[i].memsz = parent->text_segs[i].memsz;
    }
    tf_copy(&child->ctx, tf); // mismos registros que el padre en este instante...
    child->ctx.v0 = 0;        // ...salvo el retorno de fork(): 0 en el hijo
    child->ctx.epc = tf->epc + 4; // no reejecutar el syscall (igual que el padre, ver trap.c)
    // Señales a SIG_DFL en el hijo: no las hereda del padre (a diferencia
    // del fork() real de POSIX) ni del ocupante previo del slot — más
    // simple que copiarlas, y ningún test/programa de este proyecto instala
    // un handler antes de forkear, así que no hay caso real que lo necesite.
    for (int i = 0; i < NSIG; i++) {
        child->sig_handler[i] = (u32)SIG_DFL;
    }
    child->sig_trampoline = 0;
    child->sig_in_handler = 0;
    child->state = PROC_READY;

    return slot; // el padre recibe el pid del hijo (lo pisa trap.c en tf->v0)
}
