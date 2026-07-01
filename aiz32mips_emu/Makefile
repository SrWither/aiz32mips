LLVM_MC   = llvm-mc
LD_LLD    = ld.lld
OBJCOPY   = llvm-objcopy
CLANG     = clang
SRC_DIR   = data
ARCH      = mipsel
ELF_FMT   = elf32-little
LINK_FMT  = elf32ltsmip
TEXT_ADDR = 0xBFC00000
ENTRY     = _start

ASM_SRCS  := $(wildcard $(SRC_DIR)/*.s)
C_SRCS    := $(wildcard $(SRC_DIR)/*.c)
BIN_OUTS  := $(ASM_SRCS:.s=.bin) $(C_SRCS:.c=.bin)

all: $(BIN_OUTS)
	@echo "Binarios generados correctamente"

$(SRC_DIR)/%.bin: $(SRC_DIR)/%.s
	@$(LLVM_MC) -arch=$(ARCH) -filetype=obj $< -o $(SRC_DIR)/$*.o
	@$(LD_LLD) -m $(LINK_FMT) -Ttext=$(TEXT_ADDR) -e $(ENTRY) --emit-relocs --noinhibit-exec --oformat=$(ELF_FMT) $(SRC_DIR)/$*.o -o $(SRC_DIR)/$*.elf
	@$(OBJCOPY) -O binary $(SRC_DIR)/$*.elf $@
	@rm -f $(SRC_DIR)/$*.o $(SRC_DIR)/$*.elf
	@echo "$@"

# -I../kernel: gpu.h/gpu_math.h/keyboard.h se mudaron ahí (son drivers de
# MMIO, uso genuino de kernel; ver kernel/Makefile). Los .c de acá siguen
# siendo tests standalone contra la GPU/teclado reales, sin kernel de por
# medio.
$(SRC_DIR)/%.bin: $(SRC_DIR)/%.c
	@$(CLANG) -target $(ARCH) -mips32 -ffreestanding -fno-pic -fno-builtin -O2 -nostdlib -static -I../kernel -c $< -o $(SRC_DIR)/$*.o
	@$(LD_LLD) -m $(LINK_FMT) -Ttext=$(TEXT_ADDR) -e $(ENTRY) --emit-relocs --noinhibit-exec --oformat=$(ELF_FMT) $(SRC_DIR)/$*.o -o $(SRC_DIR)/$*.elf
	@$(OBJCOPY) -O binary $(SRC_DIR)/$*.elf $@
# 	@rm -f $(SRC_DIR)/$*.o $(SRC_DIR)/$*.elf
	@echo "$@"

clean:
	@rm -f $(SRC_DIR)/*.o $(SRC_DIR)/*.elf $(SRC_DIR)/*.bin
	@echo "Limpieza completa"

.PHONY: all clean
