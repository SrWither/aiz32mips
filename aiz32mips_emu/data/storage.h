// storage.h — MMIO de storage para AIZ-32 (freestanding, sin libc)
//
// Disco de bloques de 512 bytes respaldado por un archivo real del host
// (ver aiz32mips_core::devices::storage): lo que se escribe con
// storage_write_block sobrevive a que se cierre el emulador. Comandos
// síncronos, sin IRQ: se resuelven en el mismo write a REG_CMD.

#ifndef AIZ_STORAGE_H
#define AIZ_STORAGE_H

typedef unsigned char st_u8;
typedef unsigned int st_u32;

#define STORAGE_MMIO_BASE 0xBF803000u
#define STORAGE_BLOCK_SIZE 512

#define REG_STORAGE_BLOCK (*(volatile st_u32 *)(STORAGE_MMIO_BASE + 0x000)) // número de bloque
#define REG_STORAGE_CMD (*(volatile st_u32 *)(STORAGE_MMIO_BASE + 0x004)) // 1=leer 2=escribir
#define REG_STORAGE_STATUS (*(volatile st_u32 *)(STORAGE_MMIO_BASE + 0x008)) // bit0 = error
#define STORAGE_BUF ((volatile st_u8 *)(STORAGE_MMIO_BASE + 0x200)) // 512 bytes

#define STORAGE_CMD_READ 1u
#define STORAGE_CMD_WRITE 2u
#define STORAGE_STATUS_ERROR 1u

// Lee el bloque `block` del disco a `dst` (STORAGE_BLOCK_SIZE bytes).
// Devuelve 0 si salió bien, -1 si el device marcó error (bloque fuera de
// rango).
static inline int storage_read_block(st_u32 block, st_u8 *dst) {
    REG_STORAGE_BLOCK = block;
    REG_STORAGE_CMD = STORAGE_CMD_READ;
    if (REG_STORAGE_STATUS & STORAGE_STATUS_ERROR) {
        return -1;
    }
    for (st_u32 i = 0; i < STORAGE_BLOCK_SIZE; i++) {
        dst[i] = STORAGE_BUF[i];
    }
    return 0;
}

// Escribe `src` (STORAGE_BLOCK_SIZE bytes) al bloque `block` del disco.
static inline int storage_write_block(st_u32 block, const st_u8 *src) {
    for (st_u32 i = 0; i < STORAGE_BLOCK_SIZE; i++) {
        STORAGE_BUF[i] = src[i];
    }
    REG_STORAGE_BLOCK = block;
    REG_STORAGE_CMD = STORAGE_CMD_WRITE;
    return (REG_STORAGE_STATUS & STORAGE_STATUS_ERROR) ? -1 : 0;
}

#endif // AIZ_STORAGE_H
