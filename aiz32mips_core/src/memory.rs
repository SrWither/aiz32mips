use core::any::Any;
use core::ops::RangeInclusive;

#[derive(Debug, Clone, Copy)]
pub enum MemoryError {
    AddressErrorLoad(u32),
    AddressErrorStore(u32),
    Unmapped(u32),
    RomWrite(u32),
}

pub type MemResult<T> = Result<T, MemoryError>;

pub trait Device: Any {
    fn range(&self) -> RangeInclusive<u32>;

    fn read8(&mut self, paddr: u32) -> MemResult<u8>;
    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()>;

    fn read16(&mut self, paddr: u32) -> MemResult<u16> {
        let lo = self.read8(paddr)? as u16;
        let hi = self.read8(paddr + 1)? as u16;
        Ok((hi << 8) | lo)
    }

    fn read32(&mut self, paddr: u32) -> MemResult<u32> {
        let b0 = self.read8(paddr)? as u32;
        let b1 = self.read8(paddr + 1)? as u32;
        let b2 = self.read8(paddr + 2)? as u32;
        let b3 = self.read8(paddr + 3)? as u32;
        Ok((b3 << 24) | (b2 << 16) | (b1 << 8) | b0)
    }

    fn write16(&mut self, paddr: u32, value: u16) -> MemResult<()> {
        self.write8(paddr, (value & 0xFF) as u8)?;
        self.write8(paddr + 1, (value >> 8) as u8)
    }

    fn write32(&mut self, paddr: u32, value: u32) -> MemResult<()> {
        self.write8(paddr, (value & 0xFF) as u8)?;
        self.write8(paddr + 1, ((value >> 8) & 0xFF) as u8)?;
        self.write8(paddr + 2, ((value >> 16) & 0xFF) as u8)?;
        self.write8(paddr + 3, ((value >> 24) & 0xFF) as u8)
    }

    // Sin cuerpo default ni `where Self: Sized`: tienen que poder llamarse
    // a través de `dyn Device` (p.ej. para bajar de tipo un device puntual
    // desde el bus, como hace `MemoryBus::device_mut`). Cada impl concreto
    // los provee con el `{ self }` de siempre.
    fn as_any(&self) -> &dyn Any;
    fn as_any_mut(&mut self) -> &mut dyn Any;
}

pub struct MemoryBus {
    devices: Vec<Box<dyn Device>>,
    pub little_endian: bool,
}

impl MemoryBus {
    pub fn new(little_endian: bool) -> Self {
        Self {
            devices: Vec::new(),
            little_endian,
        }
    }

    pub fn add_device(&mut self, dev: Box<dyn Device>) {
        self.devices.push(dev);
    }

    /// Busca el primer device registrado de tipo concreto `T` (p.ej. para
    /// que el host pulse VBLANK en la GPU después de presentar un frame).
    pub fn device_mut<T: Device>(&mut self) -> Option<&mut T> {
        self.devices
            .iter_mut()
            .find_map(|d| d.as_any_mut().downcast_mut::<T>())
    }

    fn find_device_mut(&mut self, paddr: u32) -> Option<&mut dyn Device> {
        self.devices
            .iter_mut()
            .find(|d| d.range().contains(&paddr))
            .map(|b| b.as_mut())
    }

    pub fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        if let Some(dev) = self.find_device_mut(paddr) {
            dev.read8(paddr)
        } else {
            Err(MemoryError::Unmapped(paddr))
        }
    }

    pub fn read16(&mut self, paddr: u32) -> MemResult<u16> {
        match self.find_device_mut(paddr) {
            Some(dev) => dev.read16(paddr),
            None => Err(MemoryError::Unmapped(paddr)),
        }
    }

    pub fn read32(&mut self, paddr: u32) -> MemResult<u32> {
        match self.find_device_mut(paddr) {
            Some(dev) => dev.read32(paddr),
            None => Err(MemoryError::Unmapped(paddr)),
        }
    }

    pub fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        if let Some(dev) = self.find_device_mut(paddr) {
            dev.write8(paddr, value)
        } else {
            Err(MemoryError::Unmapped(paddr))
        }
    }

    pub fn write16(&mut self, paddr: u32, value: u16) -> MemResult<()> {
        match self.find_device_mut(paddr) {
            Some(dev) => dev.write16(paddr, value),
            None => Err(MemoryError::Unmapped(paddr)),
        }
    }

    pub fn write32(&mut self, paddr: u32, value: u32) -> MemResult<()> {
        match self.find_device_mut(paddr) {
            Some(dev) => dev.write32(paddr, value),
            None => Err(MemoryError::Unmapped(paddr)),
        }
    }

    pub fn translate_vaddr(&self, vaddr: u32) -> MemResult<u32> {
        match vaddr {
            0x0000_0000..=0x7FFF_FFFF => Ok(vaddr), // KUSEG
            0x8000_0000..=0x9FFF_FFFF => Ok(vaddr.wrapping_sub(0x8000_0000)), // KSEG0
            0xA000_0000..=0xBFFF_FFFF => Ok(vaddr.wrapping_sub(0xA000_0000)), // KSEG1
            _ => Err(MemoryError::Unmapped(vaddr)),
        }
    }

    pub fn read8_virt(&mut self, vaddr: u32) -> MemResult<u8> {
        let paddr = self.translate_vaddr(vaddr)?;
        self.read8(paddr)
    }

    pub fn read16_virt(&mut self, vaddr: u32) -> MemResult<u16> {
        if vaddr % 2 != 0 {
            return Err(MemoryError::AddressErrorLoad(vaddr));
        }
        let paddr = self.translate_vaddr(vaddr)?;
        self.read16(paddr)
    }

    pub fn read32_virt(&mut self, vaddr: u32) -> MemResult<u32> {
        if vaddr % 4 != 0 {
            return Err(MemoryError::AddressErrorLoad(vaddr));
        }
        let paddr = self.translate_vaddr(vaddr)?;
        self.read32(paddr)
    }

    pub fn write8_virt(&mut self, vaddr: u32, val: u8) -> MemResult<()> {
        let paddr = self.translate_vaddr(vaddr)?;
        self.write8(paddr, val)
    }

    pub fn write16_virt(&mut self, vaddr: u32, val: u16) -> MemResult<()> {
        if vaddr % 2 != 0 {
            return Err(MemoryError::AddressErrorStore(vaddr));
        }
        let paddr = self.translate_vaddr(vaddr)?;
        self.write16(paddr, val)
    }

    pub fn write32_virt(&mut self, vaddr: u32, val: u32) -> MemResult<()> {
        if vaddr % 4 != 0 {
            return Err(MemoryError::AddressErrorStore(vaddr));
        }
        let paddr = self.translate_vaddr(vaddr)?;
        self.write32(paddr, val)
    }
}
