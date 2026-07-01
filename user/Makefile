CLANG     = clang
LD_LLD    = ld.lld
OBJCOPY   = llvm-objcopy
ARCH      = mipsel
LINK_FMT  = elf32ltsmip
ELF_FMT   = elf32-little

# -G0: sin small-data/gp-relative (no hay $gp inicializado, ver ../kernel).
CFLAGS  = -target $(ARCH) -mips32 -ffreestanding -fno-pic -fno-builtin \
          -fomit-frame-pointer -G0 -O2 -nostdlib -static -Wall

PROGRAMS = hello pingpong cube3d

all: $(PROGRAMS:%=%.bin)

LIBC_HDRS = libc/string.h libc/stdlib.h libc/stdio.h

%.o: %.c ../kernel/abi.h ../kernel/gpu_math.h gpu_user.h $(LIBC_HDRS)
	$(CLANG) $(CFLAGS) -c $< -o $@

# -z max-page-size=4096: sin esto lld separa cada PT_LOAD a offsets de
# archivo alineados a 64KB (el default "genérico"), infla el .elf a decenas
# de KB de puro padding. Un solo proceso nuestro entra en una página de 4KB
# (ver kernel/mm.c), no hace falta esa separación.
%.elf: %.o linker.ld
	$(LD_LLD) -m $(LINK_FMT) -T linker.ld --oformat=$(ELF_FMT) -z max-page-size=4096 -o $@ $<
	@echo "$@"

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f *.o $(PROGRAMS:%=%.elf) $(PROGRAMS:%=%.bin)

# Los .elf son intermedios en la cadena .c->.o->.elf->.bin: sin esto, make
# los borra solo al terminar. kernel/Makefile los necesita enteros (no el
# .bin ya aplanado) para copiarlos al disco FAT32 en /bin.
.SECONDARY: $(PROGRAMS:%=%.elf)

.PHONY: all clean
