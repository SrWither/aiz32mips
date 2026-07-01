/* linker.ld — todo el kernel vive en kseg1 (0xBFC00000+), sin separar
 * .data/.bss a RAM: el loader del emulador (aiz32mips_core::elf) copia
 * cada PT_LOAD a su dirección física resuelta y rellena memsz-filesz con
 * ceros, así que no hace falta un crt0 que copie nada en runtime.
 *
 * .text.reset / .text.vector_general / .text.vector_tlb quedan clavados
 * en 0xBFC00000 / 0xBFC00180 / 0xBFC00200: son los vectores de reset y
 * excepción de MIPS con Status.BEV=1 (el valor por defecto tras reset,
 * que este kernel nunca cambia).
 *
 * Sin MEMORY{}: mezclar `> region` con asignaciones explícitas de "."
 * hace que ld.lld empaque las secciones ignorando los saltos de
 * dirección. A secas, el contador de ubicación sí se respeta.
 */

ENTRY(_start)

SECTIONS {
    . = 0xBFC00000;
    .text.reset : { *(.text.reset) }

    . = 0xBFC00180;
    .text.vector_general : { *(.text.vector_general) }

    . = 0xBFC00200;
    .text.vector_tlb : { *(.text.vector_tlb) }

    . = 0xBFC00280;
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

    .stack : {
        . = ALIGN(8);
        . += 0x4000;
        _stack_top = .;
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
