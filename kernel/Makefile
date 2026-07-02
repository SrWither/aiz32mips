CLANG     = clang
LD_LLD    = ld.lld
OBJCOPY   = llvm-objcopy
ARCH      = mipsel
LINK_FMT  = elf32ltsmip
ELF_FMT   = elf32-little

# -G0: sin small-data/gp-relative (no inicializamos $gp en boot.S).
# gpu.h/gpu_math.h/keyboard.h/storage.h viven acá mismo (kernel/): no hace
# falta ningún -I, el include con comillas ya los encuentra en el mismo
# directorio que el .c que los pide.
CFLAGS  = -target $(ARCH) -mips32 -ffreestanding -fno-pic -fno-builtin \
          -fomit-frame-pointer -G0 -O2 -nostdlib -static -Wall
ASFLAGS = -target $(ARCH) -mips32 -ffreestanding -fno-pic -G0 -nostdlib \
          -D__ASSEMBLER__ -c

# Artefactos de compilación separados de las fuentes (antes vivían sueltos
# acá mismo, mezclados con los .c/.h): OBJ_DIR para los .o intermedios,
# ELF_DIR para el kernel.elf final, BIN_DIR para el kernel.bin (objcopy
# plano, on-demand, ver más abajo). disk.img/diskroot quedan afuera de
# BUILD_DIR a propósito: no son un byproduct de compilar, son "el disco de
# verdad" (ver el comentario del target disk.img).
BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
ELF_DIR   = $(BUILD_DIR)/elf
BIN_DIR   = $(BUILD_DIR)/bin

C_SRCS  = kernel.c console.c trap.c shell.c mm.c sched.c sem.c fs.c
OBJS    = $(OBJ_DIR)/boot.o $(addprefix $(OBJ_DIR)/,$(C_SRCS:.c=.o))

KERNEL_ELF = $(ELF_DIR)/kernel.elf
KERNEL_BIN = $(BIN_DIR)/kernel.bin

all: $(KERNEL_ELF)

$(OBJ_DIR) $(ELF_DIR) $(BIN_DIR):
	mkdir -p $@

$(OBJ_DIR)/boot.o: boot.S kernel.h | $(OBJ_DIR)
	$(CLANG) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/%.o: %.c kernel.h | $(OBJ_DIR)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJS) linker.ld | $(ELF_DIR)
	$(LD_LLD) -m $(LINK_FMT) -T linker.ld --oformat=$(ELF_FMT) -o $@ $(OBJS)
	@echo "$@"

$(KERNEL_BIN): $(KERNEL_ELF) | $(BIN_DIR)
	$(OBJCOPY) -O binary $< $@

# Alias cortos: "make kernel.elf"/"make kernel.bin" (el nombre que ya se
# venía usando en todos lados, tests incluidos) siguen andando igual, el
# archivo real vive en build/.
kernel.elf: $(KERNEL_ELF)
kernel.bin: $(KERNEL_BIN)

# disk.img: FAT32 real (mkfs.vfat + mtools arman el estado inicial desde
# el host; fs.c ya sabe escribir por su cuenta una vez que el kernel está
# corriendo, ver fs_write). /bin lleva los .elf de userland tal cual salen
# del linker (sin objcopy a .bin: en el disco los binarios son ELF de
# punta a punta). No es parte de `all` a propósito: es un artefacto
# persistente ("el disco de verdad"), regenerarlo es una acción explícita
# (`make disk.img`), no un efecto colateral de compilar el kernel.
disk.img: FORCE diskroot/home/readme.txt
	$(MAKE) -C ../user
	rm -f disk.img
	dd if=/dev/zero of=disk.img bs=1M count=256 status=none
	mkfs.vfat -F 32 disk.img >/dev/null
	mmd -i disk.img ::/bin ::/lib ::/include ::/home
	mcopy -i disk.img ../user/build/elf/hello.elf ::/bin/hello.elf
	mcopy -i disk.img ../user/build/elf/ping.elf ::/bin/ping.elf
	mcopy -i disk.img ../user/build/elf/pingpong.elf ::/bin/pingpong.elf
	mcopy -i disk.img ../user/build/elf/cube3d.elf ::/bin/cube3d.elf
	mcopy -i disk.img ../user/build/elf/filetest.elf ::/bin/filetest.elf
	mcopy -i disk.img ../user/build/elf/memtest.elf ::/bin/memtest.elf
	mcopy -i disk.img ../user/build/elf/sigtest.elf ::/bin/sigtest.elf
	mcopy -i disk.img ../user/build/elf/fopentst.elf ::/bin/fopentst.elf
	mcopy -i disk.img ../user/build/elf/fputest.elf ::/bin/fputest.elf
	mcopy -i disk.img ../user/build/elf/audiotst.elf ::/bin/audiotst.elf
	mcopy -i disk.img ../user/build/elf/bigtest.elf ::/bin/bigtest.elf
	mcopy -i disk.img ../user/build/elf/libctest.elf ::/bin/libctest.elf
	mcopy -i disk.img diskroot/home/readme.txt ::/home/readme.txt
	@echo "disk.img"

clean:
	$(MAKE) -C ../user clean
	rm -rf $(BUILD_DIR)

.PHONY: all clean FORCE kernel.elf kernel.bin
