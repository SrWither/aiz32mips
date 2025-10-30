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
}
