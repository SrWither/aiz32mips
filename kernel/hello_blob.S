// hello_blob.S — embebe user/hello.bin (copiado a hello.bin por el
// Makefile) dentro de kernel.elf. Se ensambla con las mismas flags que
// boot.S para que el ABI/e_flags del objeto matcheen: `llvm-objcopy -I
// binary` genera un ELF con ABI incompatible (n64) y ld.lld lo rechaza.
.section .rodata
.global _binary_hello_bin_start
.global _binary_hello_bin_end
_binary_hello_bin_start:
.incbin "hello.bin"
_binary_hello_bin_end:
