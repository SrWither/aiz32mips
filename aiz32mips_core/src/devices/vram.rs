use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;

pub struct GpuVram {
    base: u32,
    data: Vec<u8>,
}

impl GpuVram {
    pub fn new(base: u32, size: usize) -> Self {
        Self { base, data: vec![0; size] }
    }

    #[inline] fn to_off(&self, paddr: u32) -> Option<usize> {
        let off = paddr.wrapping_sub(self.base) as usize;
        if off < self.data.len() { Some(off) } else { None }
    }

    pub fn range_bounds(&self) -> (u32, u32) {
        (self.base, self.base + (self.data.len() as u32) - 1)
    }

    pub fn slice_mut(&mut self) -> &mut [u8] { &mut self.data }
    pub fn slice(&self) -> &[u8] { &self.data }
}

impl Device for GpuVram {
    fn range(&self) -> RangeInclusive<u32> {
        let (lo, hi) = self.range_bounds();
        lo..=hi
    }

    fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        if let Some(off) = self.to_off(paddr) { Ok(self.data[off]) } else { Err(MemoryError::Unmapped(paddr)) }
    }
    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        if let Some(off) = self.to_off(paddr) { self.data[off] = value; Ok(()) } else { Err(MemoryError::Unmapped(paddr)) }
    }
}
