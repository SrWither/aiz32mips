// embedded.h — binarios de usuario embebidos en el propio kernel.elf.
// Los símbolos los genera `llvm-objcopy -I binary` sobre user/hello.bin
// (ver kernel/Makefile): convención estándar de objcopy para un archivo
// llamado literalmente "hello.bin", sin componentes de directorio.
#ifndef AIZ_EMBEDDED_H
#define AIZ_EMBEDDED_H

extern u8 _binary_hello_bin_start[];
extern u8 _binary_hello_bin_end[];
extern u8 _binary_pingpong_bin_start[];
extern u8 _binary_pingpong_bin_end[];

#endif // AIZ_EMBEDDED_H
