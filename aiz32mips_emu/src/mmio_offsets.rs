// Algunos quedan sin uso del lado Rust (los pokean directo los ejemplos en
// C), pero los dejamos acá para que el mapa de registros tenga una sola
// fuente de verdad documentada en el lado del host.
#![allow(dead_code)]

// Dirección física real del device en el bus (usada al registrarlo).
pub const GPU_MMIO_PHYS: u32 = 0x1F80_2000;
// Vista por el CPU en kseg1 (uncached, sin TLB) — todo este código bare
// metal corre antes de que exista ninguna entrada de TLB, así que tiene
// que vivir en un segmento sin traducción, igual que el propio vector de
// reset (0xBFC0_0000). kuseg exigiría TLB y rompería en el primer acceso.
pub const GPU_MMIO_BASE: u32 = 0xA000_0000 + GPU_MMIO_PHYS; // 0xBF80_2000

pub const REG_FB_WIDTH: u32 = GPU_MMIO_BASE + 0x00; // u16
pub const REG_FB_HEIGHT: u32 = GPU_MMIO_BASE + 0x02; // u16
pub const REG_FB0_ADDR: u32 = GPU_MMIO_BASE + 0x08; // u32, offset en VRAM
pub const REG_FB1_ADDR: u32 = GPU_MMIO_BASE + 0x0C; // u32, offset en VRAM (0 = sin double buffer)
pub const REG_ZBUF_ADDR: u32 = GPU_MMIO_BASE + 0x10; // u32, offset en VRAM (0 = sin Z-buffer)
pub const REG_STATUS: u32 = GPU_MMIO_BASE + 0x14; // u32: bit1=VBLANK bit2=DRAW_FB bit3=DISPLAY_FB
pub const REG_CMD_ADDR: u32 = GPU_MMIO_BASE + 0x18; // u32, offset en VRAM de la display list
pub const REG_CMD_LEN: u32 = GPU_MMIO_BASE + 0x1C; // u32, cantidad de words u32
pub const REG_CMD_KICK: u32 = GPU_MMIO_BASE + 0x20; // u8, cualquier escritura ejecuta
pub const REG_FONTADDR: u32 = GPU_MMIO_BASE + 0x24; // u32, offset en VRAM
pub const REG_FONTW: u32 = GPU_MMIO_BASE + 0x28; // u8
pub const REG_FONTH: u32 = GPU_MMIO_BASE + 0x29; // u8
pub const REG_TEXT_ADDR: u32 = GPU_MMIO_BASE + 0x30; // u32, offset en VRAM
pub const REG_TEXT_COLS: u32 = GPU_MMIO_BASE + 0x34; // u16
pub const REG_TEXT_ROWS: u32 = GPU_MMIO_BASE + 0x36; // u16
pub const REG_TEXT_PALETTE_ADDR: u32 = GPU_MMIO_BASE + 0x38; // u32
pub const REG_TEXT_ENABLE: u32 = GPU_MMIO_BASE + 0x3C; // u8

// ───────────────────────────── Teclado ─────────────────────────────────
pub const KBD_MMIO_PHYS: u32 = 0x1F80_1000;
pub const KBD_MMIO_BASE: u32 = 0xA000_0000 + KBD_MMIO_PHYS; // 0xBF80_1000, kseg1

pub const REG_KBD_DATA: u32 = KBD_MMIO_BASE + 0x00; // u32, leer hace pop del FIFO (0 si está vacío)
pub const REG_KBD_STATUS: u32 = KBD_MMIO_BASE + 0x04; // u32, bit0 = hay datos
pub const REG_KBD_CTRL: u32 = KBD_MMIO_BASE + 0x08; // u8, bit0 = IRQ (Cause.IP2) habilitada

// ───────────────────────────── Storage ──────────────────────────────────
// Disco de bloques respaldado por un archivo real del host (ver
// aiz32mips_core::devices::storage): lo que se escribe sobrevive a que se
// cierre el emulador. Comandos síncronos, sin IRQ.
pub const STORAGE_MMIO_PHYS: u32 = 0x1F80_3000;
pub const STORAGE_MMIO_BASE: u32 = 0xA000_0000 + STORAGE_MMIO_PHYS; // 0xBF80_3000, kseg1

pub const REG_STORAGE_BLOCK: u32 = STORAGE_MMIO_BASE + 0x00; // u32, número de bloque
pub const REG_STORAGE_CMD: u32 = STORAGE_MMIO_BASE + 0x04; // u32, 1=leer bloque→buffer 2=escribir buffer→bloque
pub const REG_STORAGE_STATUS: u32 = STORAGE_MMIO_BASE + 0x08; // u32, bit0 = error (bloque fuera de rango)
pub const REG_STORAGE_BUF: u32 = STORAGE_MMIO_BASE + 0x200; // 512 bytes, legibles/escribibles como RAM

// ───────────────────────────── Audio ────────────────────────────────────
// Un canal, PCM mono s16 a aiz32mips_core::devices::audio::SAMPLE_RATE fijo
// (ver ese módulo). Mismo patrón "submit al staging + kick" que la GPU.
pub const AUDIO_MMIO_PHYS: u32 = 0x1F80_4000;
pub const AUDIO_MMIO_BASE: u32 = 0xA000_0000 + AUDIO_MMIO_PHYS; // 0xBF80_4000, kseg1

pub const REG_AUDIO_LEN: u32 = AUDIO_MMIO_BASE + 0x00; // u32, samples válidos en AUDIO_BUF para el próximo KICK
pub const REG_AUDIO_KICK: u32 = AUDIO_MMIO_BASE + 0x04; // u8, cualquier escritura encola AUDIO_BUF[0..LEN]
pub const REG_AUDIO_STATUS: u32 = AUDIO_MMIO_BASE + 0x08; // u32, samples pendientes en la cola de reproducción
pub const REG_AUDIO_CTRL: u32 = AUDIO_MMIO_BASE + 0x0C; // u8, bit0 = enable (0 corta y vacía la cola)
pub const REG_AUDIO_BUF: u32 = AUDIO_MMIO_BASE + 0x200; // STAGING_SAMPLES x s16, legible/escribible como RAM
