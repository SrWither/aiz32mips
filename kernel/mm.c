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

static void tlb_write_pair(u32 index, u32 vaddr_even, u32 phys_even, u32 phys_odd) {
    cop0_write_index(index);
    cop0_write_entryhi((vaddr_even & ~0x1FFFu) | (index % MAX_PROCS));
    cop0_write_entrylo0(((phys_even >> 12) << 6) | (1u << 2) | (1u << 1)); // D=1,V=1
    cop0_write_entrylo1(((phys_odd >> 12) << 6) | (1u << 2) | (1u << 1));
    cop0_write_pagemask(0);
    cop0_tlbwi();
}

// Vaddr fija de todo proceso de usuario (USER_VADDR/USER_HEAP_VADDR/
// USER_STACK_VADDR, kernel.h): como el TLB matchea por (vpn2,asid) y cada
// proceso tiene su propio ASID, no hay colisión aunque todos compartan la
// misma dirección. Dos entradas de TLB por proceso (cada una cubre un
// VPN2 = 8KB, par+impar): la primera junta prog+heap0, la segunda
// heap1+stack — así entra un heap real de 8KB sin que cada proceso
// necesite más de 2 índices de TLB (con MAX_PROCS=4 son 8 de los 16 que
// tiene el core, sobra margen). El índice de la 2da entrada se corre
// MAX_PROCS lugares para no pisar la del slot de al lado; el ASID (bits
// bajos de EntryHi, no el índice) es el mismo en ambas: por eso
// tlb_write_pair calcula el ASID como `index % MAX_PROCS` en vez de
// recibirlo aparte.
void mm_map_user(u32 asid, u32 prog_phys, u32 heap0_phys, u32 heap1_phys, u32 stack_phys) {
    tlb_write_pair(asid, USER_VADDR, prog_phys, heap0_phys);
    tlb_write_pair(asid + MAX_PROCS, USER_HEAP_VADDR + 0x1000u, heap1_phys, stack_phys);
}
