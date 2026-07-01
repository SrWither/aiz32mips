CLANG     = clang
LD_LLD    = ld.lld
OBJCOPY   = llvm-objcopy
ARCH      = mipsel
LINK_FMT  = elf32ltsmip
ELF_FMT   = elf32-little

# -G0: sin small-data/gp-relative (no inicializamos $gp en boot.S).
CFLAGS  = -target $(ARCH) -mips32 -ffreestanding -fno-pic -fno-builtin \
          -fomit-frame-pointer -G0 -O2 -nostdlib -static -Wall \
          -I../aiz32mips_emu/data
ASFLAGS = -target $(ARCH) -mips32 -ffreestanding -fno-pic -G0 -nostdlib \
          -D__ASSEMBLER__ -I../aiz32mips_emu/data -c

C_SRCS  = kernel.c console.c trap.c shell.c mm.c sched.c sem.c fs.c
OBJS    = boot.o $(C_SRCS:.c=.o) hello_blob.o pingpong_blob.o

all: kernel.elf

boot.o: boot.S kernel.h
	$(CLANG) $(ASFLAGS) $< -o $@

%.o: %.c kernel.h
	$(CLANG) $(CFLAGS) -c $< -o $@

# *.bin: apps de usuario embebidas (ver embedded.h + shell.c), construidas
# standalone en ../user y copiadas acá para que *_blob.S las encuentre con
# su nombre literal (.incbin es relativo al directorio de compilación).
hello.bin pingpong.bin: FORCE
	$(MAKE) -C ../user
	cp ../user/hello.bin hello.bin
	cp ../user/pingpong.bin pingpong.bin

hello_blob.o: hello_blob.S hello.bin
	$(CLANG) $(ASFLAGS) hello_blob.S -o hello_blob.o

pingpong_blob.o: pingpong_blob.S pingpong.bin
	$(CLANG) $(ASFLAGS) pingpong_blob.S -o pingpong_blob.o

kernel.elf: $(OBJS) linker.ld
	$(LD_LLD) -m $(LINK_FMT) -T linker.ld --oformat=$(ELF_FMT) -o $@ $(OBJS)
	@echo "$@"

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

clean:
	$(MAKE) -C ../user clean
	rm -f *.o kernel.elf kernel.bin hello.bin pingpong.bin

.PHONY: all clean FORCE
