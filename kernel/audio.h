// audio.h — MMIO de audio para AIZ-32 (freestanding, sin libc).
//
// Un canal, PCM mono s16 a AUDIO_SAMPLE_RATE fijo. Mismo patrón "cargar el
// staging + KICK" que gpu.h (ver REG_CMD_ADDR/REG_CMD_LEN/REG_CMD_KICK):
// el kernel copia hasta AUDIO_BUF_SAMPLES samples al buffer propio del
// device y dispara KICK, que los encola de verdad para reproducción (ver
// aiz32mips_core/src/devices/audio.rs). Debe matchear ese archivo.
#ifndef AIZ_AUDIO_H
#define AIZ_AUDIO_H

typedef unsigned char au_u8;
typedef unsigned short au_u16;
typedef unsigned int au_u32;
typedef short au_i16;

#define AUDIO_MMIO_BASE 0xBF804000u
#define AUDIO_SAMPLE_RATE 22050u
#define AUDIO_BUF_SAMPLES 1024u // tope de un solo KICK; sys_audio_submit trocea si el proceso manda más

#define REG_AUDIO_LEN (*(volatile au_u32 *)(AUDIO_MMIO_BASE + 0x000))    // samples válidos en AUDIO_BUF
#define REG_AUDIO_KICK (*(volatile au_u8 *)(AUDIO_MMIO_BASE + 0x004))    // cualquier escritura encola
#define REG_AUDIO_STATUS (*(volatile au_u32 *)(AUDIO_MMIO_BASE + 0x008)) // samples pendientes en la cola
#define REG_AUDIO_CTRL (*(volatile au_u8 *)(AUDIO_MMIO_BASE + 0x00C))    // bit0=enable; 0 corta y vacía
#define AUDIO_BUF ((volatile au_i16 *)(AUDIO_MMIO_BASE + 0x200))         // AUDIO_BUF_SAMPLES x s16

#endif // AIZ_AUDIO_H
