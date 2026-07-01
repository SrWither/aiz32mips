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

C_SRCS  = kernel.c console.c trap.c
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

clean:
	rm -f *.o kernel.elf kernel.bin

.PHONY: all clean
