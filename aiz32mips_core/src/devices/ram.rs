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
}
