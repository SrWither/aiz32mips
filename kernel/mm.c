// mm.c — memoria física y mapeo de usuario. Sin scheduler ni multiproceso
// todavía: un solo programa de usuario a la vez, una sola entrada de TLB.
#include "kernel.h"

// Guardados por enter_user_mode (boot.S) antes de saltar a modo usuario;
// SYS_EXIT (trap.c) los usa para reescribir el TrapFrame y "volver" al
// shell como si hubiera sido un return normal.
u32 g_kexit_pc;
u32 g_kexit_sp;
u32 g_kexit_ctx[10]; // s0..s7, gp, fp

// RAM física (0x00000000+, ver aiz32mips_emu/src/main.rs): el kernel vive
// entero en ROM (kseg1), así que toda la RAM está libre para procesos.
// Bump allocator: sin free todavía, alcanza para este primer paso.
#define PAGE_SIZE 0x1000u
static u32 pmm_next = 0;

u32 pmm_alloc_page(void) {
    u32 page = pmm_next;
    pmm_next += PAGE_SIZE;
    return page;
}

// Vaddr fija de la única app de usuario que existe hoy. Página par = texto
// + datos + bss (tiene que entrar en 4KB); página impar = stack.
#define USER_VADDR 0x00400000u

void user_map_and_load(const u8 *img, u32 img_len) {
    u32 prog_phys = pmm_alloc_page();
    u32 stack_phys = pmm_alloc_page();

    // El kernel corre sin traducción propia: escribe la RAM física
    // directo por kseg0 (vaddr = paddr + 0x80000000).
    u8 *prog_dst = (u8 *)(0x80000000u + prog_phys);
    for (u32 i = 0; i < PAGE_SIZE; i++) {
        prog_dst[i] = (i < img_len) ? img[i] : 0; // resto de la página: bss
    }
    u8 *stack_dst = (u8 *)(0x80000000u + stack_phys);
    for (u32 i = 0; i < PAGE_SIZE; i++) {
        stack_dst[i] = 0;
    }

    // Una sola entrada de TLB (par/impar de 4KB = 8KB por VPN2) cubre las
    // dos páginas. Index nunca se escribe: queda en 0 desde el reset, que
    // es justo la única entrada que este kernel usa.
    cop0_write_entryhi((USER_VADDR & ~0x1FFFu)); // VPN2 | ASID=0
    cop0_write_entrylo0(((prog_phys >> 12) << 6) | (1u << 2) | (1u << 1)); // D=1,V=1
    cop0_write_entrylo1(((stack_phys >> 12) << 6) | (1u << 2) | (1u << 1));
    cop0_write_pagemask(0);
    cop0_tlbwi();
}
