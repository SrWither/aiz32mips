use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;

pub struct Ram {
    base: u32,
    data: Vec<u8>,
}

impl Ram {
    pub fn new(base: u32, size: usize) -> Self {
        Self {
            base,
            data: vec![0; size],
        }
    }

    #[inline]
    fn offset(&self, paddr: u32) -> Option<usize> {
        let off = paddr.wrapping_sub(self.base) as usize;
        if off < self.data.len() { Some(off) } else { None }
    }

    /// Como `offset`, pero valida que el rango [off, off+len) entre completo
    /// (necesario para los overrides en bloque: un acceso de 16/32 bits en
    /// el último byte del device no debe entrar en panic, debe dar Unmapped
    /// igual que hacía antes el loop byte-a-byte).
    #[inline]
    fn offset_span(&self, paddr: u32, len: usize) -> Option<usize> {
        let off = paddr.wrapping_sub(self.base) as usize;
        if off.checked_add(len)? <= self.data.len() { Some(off) } else { None }
    }
}

impl Device for Ram {
    fn range(&self) -> RangeInclusive<u32> {
        let end = self.base.wrapping_add(self.data.len() as u32 - 1);
        self.base..=end
    }

    fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        if let Some(off) = self.offset(paddr) {
            Ok(self.data[off])
        } else {
            Err(MemoryError::Unmapped(paddr))
        }
    }

    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        if let Some(off) = self.offset(paddr) {
            self.data[off] = value;
            Ok(())
        } else {
            Err(MemoryError::Unmapped(paddr))
        }
    }

    // Overrides en bloque: evitan el loop byte-a-byte del default del trait.
    // El fetch de instrucción (read32) es el camino más caliente de todo el
    // emulador, así que esto importa para *todo* el sistema, no solo la GPU.
    fn read16(&mut self, paddr: u32) -> MemResult<u16> {
        let off = self.offset_span(paddr, 2).ok_or(MemoryError::Unmapped(paddr))?;
        Ok(u16::from_le_bytes(self.data[off..off + 2].try_into().unwrap()))
    }

    fn read32(&mut self, paddr: u32) -> MemResult<u32> {
        let off = self.offset_span(paddr, 4).ok_or(MemoryError::Unmapped(paddr))?;
        Ok(u32::from_le_bytes(self.data[off..off + 4].try_into().unwrap()))
    }

    fn write16(&mut self, paddr: u32, value: u16) -> MemResult<()> {
        let off = self.offset_span(paddr, 2).ok_or(MemoryError::Unmapped(paddr))?;
        self.data[off..off + 2].copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn write32(&mut self, paddr: u32, value: u32) -> MemResult<()> {
        let off = self.offset_span(paddr, 4).ok_or(MemoryError::Unmapped(paddr))?;
        self.data[off..off + 4].copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn as_any(&self) -> &dyn core::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn core::any::Any {
        self
    }
}
