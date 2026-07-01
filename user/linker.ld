/* linker.ld — programa de usuario: vive en kuseg, mapeado por el kernel
 * vía TLB a una página física (ver kernel/mm.c::user_map_and_load). Sin
 * vectores de excepción (nunca corre en modo kernel) ni .stack: el stack lo
 * arma el kernel en la página impar del mismo VPN2 antes del eret.
 */

ENTRY(_start)

SECTIONS {
    . = 0x00400000;
    .text : { *(.text) *(.text.*) }
    .rodata : { *(.rodata) *(.rodata.*) }
    .data : { *(.data) *(.data.*) }
    .sdata : { *(.sdata) *(.sdata.*) }
    .got : { *(.got) *(.got.*) }

    .bss : {
        *(.sbss) *(.sbss.*)
        *(.bss) *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.MIPS.abiflags)
        *(.reginfo)
        *(.comment)
        *(.pdr)
        *(.mdebug*)
        *(.rel.*)
        *(.note*)
    }
}
