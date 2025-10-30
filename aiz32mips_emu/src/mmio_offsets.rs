pub const GPU_MMIO_BASE: u32 = 0x1F80_2000;

pub const REG_WIDTH: u32 = GPU_MMIO_BASE + 0x00; // u16
pub const REG_HEIGHT: u32 = GPU_MMIO_BASE + 0x02; // u16
pub const REG_PITCH: u32 = GPU_MMIO_BASE + 0x04; // u16 (en píxeles cuando bpp=32)
pub const REG_BPP: u32 = GPU_MMIO_BASE + 0x06; // u8  (soportamos 32 por ahora)
pub const REG_FBADDR: u32 = GPU_MMIO_BASE + 0x08; // u32 (offset dentro de VRAM)
pub const REG_STATUS: u32 = GPU_MMIO_BASE + 0x0C; // u32 (bit0=BUSY)
pub const REG_CMD: u32 = GPU_MMIO_BASE + 0x10; // u32 (escribir comando aquí)
pub const REG_FONTADDR: u32 = GPU_MMIO_BASE + 0x20; // u32 (offset dentro de VRAM)