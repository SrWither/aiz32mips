// pingpong_blob.S — embebe user/pingpong.bin (copiado a pingpong.bin por
// el Makefile) dentro de kernel.elf. Mismo motivo que hello_blob.S: se
// ensambla con las mismas flags que boot.S para que el ABI/e_flags
// matcheen (`llvm-objcopy -I binary` genera un ELF con ABI incompatible).
.section .rodata
.global _binary_pingpong_bin_start
.global _binary_pingpong_bin_end
_binary_pingpong_bin_start:
.incbin "pingpong.bin"
_binary_pingpong_bin_end:
