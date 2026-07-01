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

// Vaddr fija de todo proceso de usuario: como el TLB matchea por
// (vpn2,asid) y cada proceso tiene su propio ASID, no hay colisión aunque
// todos compartan la misma dirección. Página par = texto+datos+bss (tiene
// que entrar en 4KB); página impar = stack.
#define USER_VADDR 0x00400000u

void mm_map_user(u32 asid, u32 prog_phys, u32 stack_phys) {
    // Una sola entrada de TLB (par/impar de 4KB = 8KB por VPN2) cubre las
    // dos páginas. Índice == ASID == slot de la tabla de procesos
    // (sched.c): se escribe una sola vez al spawnear, nunca se reescribe
    // en cada cambio de contexto.
    cop0_write_index(asid);
    cop0_write_entryhi((USER_VADDR & ~0x1FFFu) | asid);
    cop0_write_entrylo0(((prog_phys >> 12) << 6) | (1u << 2) | (1u << 1)); // D=1,V=1
    cop0_write_entrylo1(((stack_phys >> 12) << 6) | (1u << 2) | (1u << 1));
    cop0_write_pagemask(0);
    cop0_tlbwi();
}
