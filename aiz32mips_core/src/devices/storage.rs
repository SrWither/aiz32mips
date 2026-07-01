use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::Path;

pub const BLOCK_SIZE: usize = 512;
pub const BLOCK_COUNT: usize = 2048; // 1MB de disco
pub const DISK_SIZE: usize = BLOCK_SIZE * BLOCK_COUNT;

const CMD_READ: u32 = 1;
const CMD_WRITE: u32 = 2;
const STATUS_ERROR: u32 = 1 << 0;

/// Disco de bloques respaldado por un archivo real del host: lo que se
/// escribe sobrevive a que se cierre el emulador. Comandos síncronos, sin
/// IRQ ni latencia simulada — se resuelven en el mismo `write` a REG_CMD,
/// igual que ya hace REG_CMD_KICK de la GPU.
pub struct StorageMmio {
    base: u32,
    file: File,
    data: Vec<u8>, // cache completo del disco; se sincroniza al file en cada write
    block: u32,
    status: u32,
    buf: [u8; BLOCK_SIZE],
}

impl StorageMmio {
    pub fn new(base: u32, path: &Path) -> io::Result<Self> {
        let existed = path.exists();
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .open(path)?;

        let mut data = vec![0u8; DISK_SIZE];
        if existed {
            file.read(&mut data)?; // si el archivo es más chico, el resto queda en 0
        } else {
            file.write_all(&data)?; // crea el archivo del tamaño correcto, todo en cero
        }

        Ok(Self {
            base,
            file,
            data,
            block: 0,
            status: 0,
            buf: [0u8; BLOCK_SIZE],
        })
    }

    fn do_read(&mut self) {
        match self.data.get(self.block as usize * BLOCK_SIZE..).and_then(|s| s.get(..BLOCK_SIZE)) {
            Some(src) => {
                self.buf.copy_from_slice(src);
                self.status = 0;
            }
            None => self.status = STATUS_ERROR,
        }
    }

    fn do_write(&mut self) {
        let off = self.block as usize * BLOCK_SIZE;
        if off + BLOCK_SIZE > self.data.len() {
            self.status = STATUS_ERROR;
            return;
        }
        self.data[off..off + BLOCK_SIZE].copy_from_slice(&self.buf);
        // Persistencia inmediata: sin esto, cerrar el emulador sin un paso
        // de "shutdown" explícito perdería todo lo escrito.
        if self.file.seek(SeekFrom::Start(off as u64)).is_ok() {
            let _ = self.file.write_all(&self.buf);
        }
        self.status = 0;
    }

    #[inline]
    fn within(&self, paddr: u32) -> Option<u32> {
        let off = paddr.wrapping_sub(self.base);
        if off < 0x200 + BLOCK_SIZE as u32 { Some(off) } else { None }
    }
}

impl Device for StorageMmio {
    fn range(&self) -> RangeInclusive<u32> {
        self.base..=self.base + 0x200 + BLOCK_SIZE as u32 - 1
    }

    fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        Ok(match off {
            0x200..=0x3FF => self.buf[(off - 0x200) as usize],
            _ => 0, // registros de control: solo se leen enteros (ver read32)
        })
    }

    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        if let 0x200..=0x3FF = off {
            self.buf[(off - 0x200) as usize] = value;
        }
        Ok(())
    }

    fn read32(&mut self, paddr: u32) -> MemResult<u32> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        Ok(match off {
            0x00 => self.block,
            0x08 => self.status,
            0x200..=0x3FF => {
                let i = (off - 0x200) as usize;
                u32::from_le_bytes(self.buf[i..i + 4].try_into().unwrap())
            }
            _ => 0,
        })
    }

    fn write32(&mut self, paddr: u32, value: u32) -> MemResult<()> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        match off {
            0x00 => self.block = value,
            0x04 => match value {
                CMD_READ => self.do_read(),
                CMD_WRITE => self.do_write(),
                _ => {}
            },
            0x200..=0x3FF => {
                let i = (off - 0x200) as usize;
                self.buf[i..i + 4].copy_from_slice(&value.to_le_bytes());
            }
            _ => {}
        }
        Ok(())
    }

    fn as_any(&self) -> &dyn core::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn core::any::Any {
        self
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::env;

    fn temp_path(name: &str) -> std::path::PathBuf {
        let mut p = env::temp_dir();
        p.push(format!("aiz32_storage_test_{}_{}", name, std::process::id()));
        p
    }

    #[test]
    fn write_then_read_roundtrips_through_buffer() {
        let path = temp_path("roundtrip");
        let _ = std::fs::remove_file(&path);
        let mut dev = StorageMmio::new(0x1000, &path).unwrap();

        dev.write32(0x1000, 5).unwrap(); // REG_BLOCK = 5
        dev.write32(0x1200, 0xCAFEBABE).unwrap(); // primeras 4 bytes del buffer
        dev.write32(0x1004, CMD_WRITE).unwrap(); // REG_CMD = write

        dev.write32(0x1200, 0).unwrap(); // ensuciar el buffer
        dev.write32(0x1004, CMD_READ).unwrap(); // REG_CMD = read (vuelve a traer el bloque 5)
        assert_eq!(dev.read32(0x1200).unwrap(), 0xCAFEBABE);

        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn persists_across_separate_instances() {
        let path = temp_path("persist");
        let _ = std::fs::remove_file(&path);
        {
            let mut dev = StorageMmio::new(0x1000, &path).unwrap();
            dev.write32(0x1000, 10).unwrap();
            dev.write32(0x1200, 0x11223344).unwrap();
            dev.write32(0x1004, CMD_WRITE).unwrap();
        }
        {
            let mut dev = StorageMmio::new(0x1000, &path).unwrap();
            dev.write32(0x1000, 10).unwrap();
            dev.write32(0x1004, CMD_READ).unwrap();
            assert_eq!(dev.read32(0x1200).unwrap(), 0x11223344);
        }
        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn out_of_range_block_sets_error_status() {
        let path = temp_path("oob");
        let _ = std::fs::remove_file(&path);
        let mut dev = StorageMmio::new(0x1000, &path).unwrap();
        dev.write32(0x1000, BLOCK_COUNT as u32).unwrap(); // fuera de rango
        dev.write32(0x1004, CMD_READ).unwrap();
        assert_eq!(dev.read32(0x1008).unwrap(), STATUS_ERROR);
        std::fs::remove_file(&path).ok();
    }
}
