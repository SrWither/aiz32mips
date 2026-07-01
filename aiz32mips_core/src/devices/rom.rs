use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;

pub struct Rom {
    base: u32,
    data: Vec<u8>,
}

impl Rom {
    pub fn new(base: u32, data: Vec<u8>) -> Self {
        Self { base, data }
    }

    #[inline]
    fn offset(&self, paddr: u32) -> Option<usize> {
        let off = paddr.wrapping_sub(self.base) as usize;
        if off < self.data.len() { Some(off) } else { None }
    }

    #[inline]
    fn offset_span(&self, paddr: u32, len: usize) -> Option<usize> {
        let off = paddr.wrapping_sub(self.base) as usize;
        if off.checked_add(len)? <= self.data.len() { Some(off) } else { None }
    }
}

impl Device for Rom {
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

    fn write8(&mut self, paddr: u32, _value: u8) -> MemResult<()> {
        Err(MemoryError::RomWrite(paddr))
    }

    fn read16(&mut self, paddr: u32) -> MemResult<u16> {
        let off = self.offset_span(paddr, 2).ok_or(MemoryError::Unmapped(paddr))?;
        Ok(u16::from_le_bytes(self.data[off..off + 2].try_into().unwrap()))
    }

    fn read32(&mut self, paddr: u32) -> MemResult<u32> {
        let off = self.offset_span(paddr, 4).ok_or(MemoryError::Unmapped(paddr))?;
        Ok(u32::from_le_bytes(self.data[off..off + 4].try_into().unwrap()))
    }

    fn as_any(&self) -> &dyn core::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn core::any::Any {
        self
    }
}
