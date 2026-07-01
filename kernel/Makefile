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

C_SRCS  = kernel.c console.c trap.c shell.c mm.c sched.c sem.c fs.c
OBJS    = boot.o $(C_SRCS:.c=.o)

all: kernel.elf

boot.o: boot.S kernel.h
	$(CLANG) $(ASFLAGS) $< -o $@

%.o: %.c kernel.h
	$(CLANG) $(CFLAGS) -c $< -o $@

kernel.elf: $(OBJS) linker.ld
	$(LD_LLD) -m $(LINK_FMT) -T linker.ld --oformat=$(ELF_FMT) -o $@ $(OBJS)
	@echo "$@"

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

# disk.img: FAT32 real (mkfs.vfat + mtools, no lo arma el kernel: el
# write-path de FAT32 todavía no existe del lado del kernel, ver fs.c).
# /bin lleva los .elf de userland tal cual salen del linker (sin objcopy a
# .bin: en el disco los binarios son ELF de punta a punta). No es parte de
# `all` a propósito: es un artefacto persistente ("el disco de verdad"),
# regenerarlo es una acción explícita (`make disk.img`), no un efecto
# colateral de compilar el kernel.
disk.img: FORCE diskroot/home/readme.txt
	$(MAKE) -C ../user
	rm -f disk.img
	dd if=/dev/zero of=disk.img bs=1M count=256 status=none
	mkfs.vfat -F 32 disk.img >/dev/null
	mmd -i disk.img ::/bin ::/lib ::/include ::/home
	mcopy -i disk.img ../user/hello.elf ::/bin/hello.elf
	mcopy -i disk.img ../user/pingpong.elf ::/bin/pingpong.elf
	mcopy -i disk.img ../user/cube3d.elf ::/bin/cube3d.elf
	mcopy -i disk.img diskroot/home/readme.txt ::/home/readme.txt
	@echo "disk.img"

clean:
	$(MAKE) -C ../user clean
	rm -f *.o kernel.elf kernel.bin

.PHONY: all clean FORCE
