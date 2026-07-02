CLANG     = clang
LD_LLD    = ld.lld
OBJCOPY   = llvm-objcopy
ARCH      = mipsel
LINK_FMT  = elf32ltsmip
ELF_FMT   = elf32-little

# -G0: sin small-data/gp-relative (no hay $gp inicializado, ver ../kernel).
CFLAGS  = -target $(ARCH) -mips32 -ffreestanding -fno-pic -fno-builtin \
          -fomit-frame-pointer -G0 -O2 -nostdlib -static -Wall

PROGRAMS = hello ping pingpong cube3d filetest memtest sigtest fopentst fputest audiotst bigtest libctest waittest

# Artefactos de compilación separados de las fuentes (antes vivían sueltos
# acá mismo, mezclados con los .c/libc/): OBJ_DIR para los .o intermedios,
# ELF_DIR para los .elf (lo que termina copiado a /bin en el disco, ver
# ../kernel/Makefile), BIN_DIR para los .bin (objcopy plano, no se usan
# para el disco pero quedan por si hacen falta sueltos).
BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
ELF_DIR   = $(BUILD_DIR)/elf
BIN_DIR   = $(BUILD_DIR)/bin

all: $(addprefix $(BIN_DIR)/,$(PROGRAMS:%=%.bin))

LIBC_HDRS = libc/string.h libc/stdlib.h libc/stdio.h libc/unistd.h libc/signal.h libc/audio.h libc/ctype.h libc/assert.h

$(OBJ_DIR) $(ELF_DIR) $(BIN_DIR):
	mkdir -p $@

$(OBJ_DIR)/%.o: %.c ../kernel/abi.h ../kernel/gpu_math.h gpu_user.h $(LIBC_HDRS) | $(OBJ_DIR)
	$(CLANG) $(CFLAGS) -c $< -o $@

# -z max-page-size=4096: sin esto lld separa cada PT_LOAD a offsets de
# archivo alineados a 64KB (el default "genérico"), infla el .elf a decenas
# de KB de puro padding. Nuestras páginas son de 4KB (ver kernel/mm.c), no
# hace falta esa separación tan grande (aplica aunque el proceso ya no
# esté limitado a una sola página, ver USER_TEXT_SIZE en kernel.h).
$(ELF_DIR)/%.elf: $(OBJ_DIR)/%.o linker.ld | $(ELF_DIR)
	$(LD_LLD) -m $(LINK_FMT) -T linker.ld --oformat=$(ELF_FMT) -z max-page-size=4096 -o $@ $<
	@echo "$@"

$(BIN_DIR)/%.bin: $(ELF_DIR)/%.elf | $(BIN_DIR)
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD_DIR)

# Los .elf son intermedios en la cadena .c->.o->.elf->.bin: sin esto, make
# los borra solo al terminar. kernel/Makefile los necesita enteros (no el
# .bin ya aplanado) para copiarlos al disco FAT32 en /bin.
.SECONDARY: $(addprefix $(ELF_DIR)/,$(PROGRAMS:%=%.elf))

.PHONY: all clean
