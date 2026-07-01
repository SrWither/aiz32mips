use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;
use std::cell::RefCell;
use std::rc::Rc;

/// Buffer de VRAM compartido entre el device de bus (acceso del CPU vía
/// loads/stores normales) y la GPU (que dibuja directamente sobre él al
/// ejecutar comandos). Antes esto se resolvía con un puntero crudo aliasing
/// un struct que además se movía de lugar (UB); con Rc<RefCell<>> ambos
/// lados comparten la misma asignación de forma segura.
pub struct VramBuffer {
    pub data: Vec<u8>,
}

impl VramBuffer {
    pub fn new(size: usize) -> Self {
        Self {
            data: vec![0; size],
        }
    }
}

pub type SharedVram = Rc<RefCell<VramBuffer>>;

pub fn new_shared_vram(size: usize) -> SharedVram {
    Rc::new(RefCell::new(VramBuffer::new(size)))
}

/// Wrapper que expone la VRAM compartida como un Device más en el bus
/// (rango de direcciones físicas, ej. 0x1000_0000..).
pub struct GpuVram {
    base: u32,
    buf: SharedVram,
}

impl GpuVram {
    pub fn new(base: u32, buf: SharedVram) -> Self {
        Self { base, buf }
    }

    #[inline]
    fn offset_span(&self, paddr: u32, len: usize) -> Option<usize> {
        let off = paddr.wrapping_sub(self.base) as usize;
        if off.checked_add(len)? <= self.buf.borrow().data.len() {
            Some(off)
        } else {
            None
        }
    }

    pub fn range_bounds(&self) -> (u32, u32) {
        (self.base, self.base + (self.buf.borrow().data.len() as u32) - 1)
    }
}

impl Device for GpuVram {
    fn range(&self) -> RangeInclusive<u32> {
        let (lo, hi) = self.range_bounds();
        lo..=hi
    }

    fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        let off = self.offset_span(paddr, 1).ok_or(MemoryError::Unmapped(paddr))?;
        Ok(self.buf.borrow().data[off])
    }

    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        let off = self.offset_span(paddr, 1).ok_or(MemoryError::Unmapped(paddr))?;
        self.buf.borrow_mut().data[off] = value;
        Ok(())
    }

    fn read16(&mut self, paddr: u32) -> MemResult<u16> {
        let off = self.offset_span(paddr, 2).ok_or(MemoryError::Unmapped(paddr))?;
        let b = &self.buf.borrow().data[off..off + 2];
        Ok(u16::from_le_bytes(b.try_into().unwrap()))
    }

    fn write16(&mut self, paddr: u32, value: u16) -> MemResult<()> {
        let off = self.offset_span(paddr, 2).ok_or(MemoryError::Unmapped(paddr))?;
        self.buf.borrow_mut().data[off..off + 2].copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn read32(&mut self, paddr: u32) -> MemResult<u32> {
        let off = self.offset_span(paddr, 4).ok_or(MemoryError::Unmapped(paddr))?;
        let b = &self.buf.borrow().data[off..off + 4];
        Ok(u32::from_le_bytes(b.try_into().unwrap()))
    }

    fn write32(&mut self, paddr: u32, value: u32) -> MemResult<()> {
        let off = self.offset_span(paddr, 4).ok_or(MemoryError::Unmapped(paddr))?;
        self.buf.borrow_mut().data[off..off + 4].copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn as_any(&self) -> &dyn core::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn core::any::Any {
        self
    }
}
