// mm.c — memoria física + mecánica de TLB. La política de "qué proceso va
// en qué slot" vive en sched.c; acá solo hay páginas y la escritura de una
// entrada de TLB a un índice/ASID dado.
#include "kernel.h"

// RAM física (0x00000000+, ver aiz32mips_emu/src/main.rs): el kernel vive
// entero en ROM (kseg1), así que toda la RAM está libre para procesos.
// Bump allocator: sin free todavía, alcanza para este paso.
#define PAGE_SIZE 0x1000u
static u32 pmm_next = 0;

u32 pmm_alloc_page(void) {
    u32 page = pmm_next;
    pmm_next += PAGE_SIZE;
    return page;
}

void pmm_write_page(u32 phys, const u8 *data, u32 len) {
    // El kernel corre sin traducción propia: escribe la RAM física
    // directo por kseg0 (vaddr = paddr + 0x80000000).
    u8 *dst = (u8 *)(0x80000000u + phys);
    for (u32 i = 0; i < PAGE_SIZE; i++) {
        dst[i] = (i < len) ? data[i] : 0; // resto de la página: bss/stack en 0
    }
}

// A diferencia de pmm_write_page, no toca el resto de la página: la usa
// sched.c para volcar cada PT_LOAD de un ELF a su offset dentro de la
// misma página física (varios segmentos comparten página, así que zero-
// llenarla en cada llamada pisaría los segmentos ya copiados).
void pmm_write_page_at(u32 phys, u32 offset, const u8 *data, u32 len) {
    u8 *dst = (u8 *)(0x80000000u + phys + offset);
    for (u32 i = 0; i < len; i++) {
        dst[i] = data[i];
    }
}

// Copia una página física entera a otra: la usa sched_fork para duplicar
// las 4 páginas del padre en las del hijo. Nada de copy-on-write todavía
// (sería lo suyo con más TLB refill del que hay hoy, ver el comentario de
// tlb.rs en aiz32mips_core) — fork acá es sinónimo de "copiar 16KB".
void pmm_copy_page(u32 dst_phys, u32 src_phys) {
    const u8 *src = (const u8 *)(0x80000000u + src_phys);
    u8 *dst = (u8 *)(0x80000000u + dst_phys);
    for (u32 i = 0; i < PAGE_SIZE; i++) {
        dst[i] = src[i];
    }
}

static void tlb_write_pair(u32 index, u32 asid, u32 vaddr_even, u32 phys_even, u32 phys_odd) {
    cop0_write_index(index);
    cop0_write_entryhi((vaddr_even & ~0x1FFFu) | asid);
    cop0_write_entrylo0(((phys_even >> 12) << 6) | (1u << 2) | (1u << 1)); // D=1,V=1
    cop0_write_entrylo1(((phys_odd >> 12) << 6) | (1u << 2) | (1u << 1));
    cop0_write_pagemask(0);
    cop0_tlbwi();
}

// Arranca el TLB para demand paging: los primeros MAX_PROCS índices (uno
// por slot de proceso, ver mm_map_user) quedan *wired* — tlbwr nunca los
// toca — y el resto (MAX_PROCS..TLB_ENTRIES-1) queda libre para que
// mm_handle_page_fault los reparta entre TODOS los procesos a medida que
// van tocando su heap. Se llama una sola vez, al boot (ver kernel.c).
void mm_init(void) {
    cop0_write_wired(MAX_PROCS);
}

// Vaddr fija de todo proceso de usuario (USER_VADDR/USER_STACK_VADDR,
// kernel.h): como el TLB matchea por (vpn2,asid) y cada proceso tiene su
// propio ASID, no hay colisión aunque todos compartan la misma dirección.
// Una sola entrada wired por proceso (índice == ASID == slot, fijo desde
// que se crea): cubre texto (par) y stack (impar) del mismo VPN2. El heap
// NO se mapea acá — es grande y con demanda de páginas, ver
// mm_handle_page_fault más abajo.
void mm_map_user(u32 asid, u32 prog_phys, u32 stack_phys) {
    tlb_write_pair(asid, asid, USER_VADDR, prog_phys, stack_phys);
}

// ─────────────────────────── heap con demanda de páginas ───────────────
// Tabla de páginas por proceso: sin esto, una página de heap que se cae
// del TLB (tlbwr la reemplaza tarde o temprano — el rango no-wired lo
// comparten TODOS los procesos) y se vuelve a tocar se confundiría con
// "primera vez" y se pisaría con ceros, perdiendo lo que hubiera. Acá
// queda la posta de qué física le corresponde a cada página virtual,
// independiente de si en este momento hay o no una entrada de TLB
// cacheándola.
#define HEAP_PAGES (USER_HEAP_SIZE / PAGE_SIZE)
static u32 heap_page_table[MAX_PROCS][HEAP_PAGES]; // 0 = todavía no tocada

static u32 heap_page_index(u32 vaddr) {
    return (vaddr - USER_HEAP_VADDR) / PAGE_SIZE;
}

// Física ya asignada a `vaddr` dentro del heap de `asid`, o la asigna
// (página nueva, en cero) si es la primera vez que se toca.
static u32 heap_page_get_or_alloc(u32 asid, u32 vaddr) {
    u32 idx = heap_page_index(vaddr);
    u32 phys = heap_page_table[asid][idx];
    if (!phys) {
        phys = pmm_alloc_page();
        pmm_write_page(phys, 0, 0);
        heap_page_table[asid][idx] = phys;
    }
    return phys;
}

// TLB refill/inválida en `badvaddr`: si cae dentro del heap del proceso
// actual, le arma una entrada (posiblemente pisando la de otro proceso
// que ya estaba usando ese índice — por eso el software, no el TLB, es la
// fuente de verdad de qué física le toca a cada página) y devuelve 1 para
// que kernel_trap reintente la instrucción que voló. 0 si `badvaddr` no
// es del heap: ahí sí es un bug de verdad del proceso.
int mm_handle_page_fault(u32 badvaddr) {
    if (badvaddr < USER_HEAP_VADDR || badvaddr >= USER_HEAP_VADDR + USER_HEAP_SIZE) {
        return 0;
    }
    u32 asid = (u32)sched_current_pid();
    u32 vpn2_even = badvaddr & ~0x1FFFu;
    u32 vpn2_odd = vpn2_even + PAGE_SIZE;

    u32 even_phys = heap_page_get_or_alloc(asid, vpn2_even);
    // La otra mitad del par (par/impar de 4KB) puede quedar fuera del
    // heap si `badvaddr` cayó en la última página y el tamaño no es
    // múltiplo de 8KB — no debería pasar con USER_HEAP_SIZE en MB, pero
    // por las dudas no se sale del array.
    u32 odd_phys = (vpn2_odd < USER_HEAP_VADDR + USER_HEAP_SIZE) ? heap_page_get_or_alloc(asid, vpn2_odd)
                                                                   : even_phys;

    cop0_write_entryhi(vpn2_even | asid);
    cop0_write_entrylo0(((even_phys >> 12) << 6) | (1u << 2) | (1u << 1));
    cop0_write_entrylo1(((odd_phys >> 12) << 6) | (1u << 2) | (1u << 1));
    cop0_write_pagemask(0);
    cop0_tlbwr(); // a un índice no-wired: puede pisar la entrada de heap de OTRO proceso, a propósito
    return 1;
}

// La llaman sched_spawn/sched_fork antes de darle `asid` a un proceso
// nuevo: sin esto, un slot reciclado (mismo ASID que un proceso ya
// muerto) heredaría por accidente tanto la tabla de páginas como
// cualquier entrada de TLB todavía viva de su antecesor — el nuevo dueño
// terminaría leyendo memoria de otro proceso en vez de páginas limpias.
void mm_reset_heap(u32 asid) {
    for (u32 i = 0; i < HEAP_PAGES; i++) {
        heap_page_table[asid][i] = 0;
    }
    for (u32 idx = MAX_PROCS; idx < TLB_ENTRIES; idx++) {
        cop0_write_index(idx);
        cop0_tlbr();
        if ((cop0_read_entryhi() & 0xFFu) == asid) {
            // ASID 0xFF: no lo usa ningún proceso real (MAX_PROCS<<0xFF),
            // así que esta entrada no va a volver a matchear por las
            // dudas de que algo la lea antes de que tlbwr la reemplace.
            cop0_write_entryhi(0xFFu);
            cop0_write_entrylo0(0);
            cop0_write_entrylo1(0);
            cop0_tlbwi();
        }
    }
}

// La llama sched_fork después de mm_reset_heap(child_asid): copia (física
// de verdad, sin copy-on-write, mismo criterio que el resto del proyecto)
// cada página de heap que el padre ya hubiera tocado.
void mm_fork_heap(u32 child_asid, u32 parent_asid) {
    for (u32 i = 0; i < HEAP_PAGES; i++) {
        u32 parent_phys = heap_page_table[parent_asid][i];
        if (parent_phys) {
            u32 child_phys = pmm_alloc_page();
            pmm_copy_page(child_phys, parent_phys);
            heap_page_table[child_asid][i] = child_phys;
        }
    }
}
