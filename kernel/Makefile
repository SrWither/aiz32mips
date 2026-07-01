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

C_SRCS  = kernel.c console.c trap.c shell.c mm.c
OBJS    = boot.o $(C_SRCS:.c=.o) hello_blob.o

all: kernel.elf

boot.o: boot.S kernel.h
	$(CLANG) $(ASFLAGS) $< -o $@

%.o: %.c kernel.h
	$(CLANG) $(CFLAGS) -c $< -o $@

# hello.bin: app de usuario embebida (ver embedded.h + shell.c), construida
# standalone en ../user y copiada acá para que hello_blob.S la encuentre
# como "hello.bin" (.incbin es relativo al directorio de compilación).
hello.bin: FORCE
	$(MAKE) -C ../user
	cp ../user/hello.bin hello.bin

hello_blob.o: hello_blob.S hello.bin
	$(CLANG) $(ASFLAGS) hello_blob.S -o hello_blob.o

kernel.elf: $(OBJS) linker.ld
	$(LD_LLD) -m $(LINK_FMT) -T linker.ld --oformat=$(ELF_FMT) -o $@ $(OBJS)
	@echo "$@"

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

clean:
	$(MAKE) -C ../user clean
	rm -f *.o kernel.elf kernel.bin hello.bin

.PHONY: all clean FORCE
