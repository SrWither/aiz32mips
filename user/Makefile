CLANG     = clang
LD_LLD    = ld.lld
OBJCOPY   = llvm-objcopy
ARCH      = mipsel
LINK_FMT  = elf32ltsmip
ELF_FMT   = elf32-little

# -G0: sin small-data/gp-relative (no hay $gp inicializado, ver ../kernel).
CFLAGS  = -target $(ARCH) -mips32 -ffreestanding -fno-pic -fno-builtin \
          -fomit-frame-pointer -G0 -O2 -nostdlib -static -Wall

C_SRCS  = hello.c
OBJS    = $(C_SRCS:.c=.o)

all: hello.bin

%.o: %.c ../kernel/abi.h
	$(CLANG) $(CFLAGS) -c $< -o $@

hello.elf: $(OBJS) linker.ld
	$(LD_LLD) -m $(LINK_FMT) -T linker.ld --oformat=$(ELF_FMT) -o $@ $(OBJS)
	@echo "$@"

hello.bin: hello.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f *.o hello.elf hello.bin

.PHONY: all clean
