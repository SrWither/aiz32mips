CLANG     = clang
LD_LLD    = ld.lld
OBJCOPY   = llvm-objcopy
ARCH      = mipsel
LINK_FMT  = elf32ltsmip
ELF_FMT   = elf32-little

# -G0: sin small-data/gp-relative (no hay $gp inicializado, ver ../kernel).
CFLAGS  = -target $(ARCH) -mips32 -ffreestanding -fno-pic -fno-builtin \
          -fomit-frame-pointer -G0 -O2 -nostdlib -static -Wall

PROGRAMS = hello ping pingpong cube3d filetest memtest sigtest fopentst fputest audiotst bigtest

all: $(PROGRAMS:%=%.bin)

LIBC_HDRS = libc/string.h libc/stdlib.h libc/stdio.h libc/unistd.h libc/signal.h libc/audio.h

%.o: %.c ../kernel/abi.h ../kernel/gpu_math.h gpu_user.h $(LIBC_HDRS)
	$(CLANG) $(CFLAGS) -c $< -o $@

# -z max-page-size=4096: sin esto lld separa cada PT_LOAD a offsets de
# archivo alineados a 64KB (el default "genérico"), infla el .elf a decenas
# de KB de puro padding. Nuestras páginas son de 4KB (ver kernel/mm.c), no
# hace falta esa separación tan grande (aplica aunque el proceso ya no
# esté limitado a una sola página, ver USER_TEXT_SIZE en kernel.h).
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
