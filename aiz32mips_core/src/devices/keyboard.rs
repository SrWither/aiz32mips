use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;
use std::collections::VecDeque;

// ───────────────────────────── formato de evento (u32) ─────────────────────
// bit31=kind (0=tecla cruda, 1=carácter de texto ya traducido por el host
// según layout/shift, vía SDL TextInput).
//
// kind=0 (tecla cruda, para juegos / lectura de estado):
//   bits0-7  = keycode interno (ver KC_*; para teclas imprimibles coincide
//              con el código ASCII sin shift, p.ej. 'a'=0x61)
//   bit8     = 1 pressed / 0 released
//   bits9-12 = modificadores: bit9=shift bit10=ctrl bit11=alt bit12=gui
//
// kind=1 (texto, para consola/shell): bits0-7 = byte ASCII ya traducido.
pub const EVT_KIND_TEXT: u32 = 1 << 31;
pub const EVT_PRESSED: u32 = 1 << 8;
pub const MOD_SHIFT: u32 = 1 << 9;
pub const MOD_CTRL: u32 = 1 << 10;
pub const MOD_ALT: u32 = 1 << 11;
pub const MOD_GUI: u32 = 1 << 12;

// Teclas especiales sin equivalente ASCII directo (las imprimibles usan su
// propio código ASCII sin shift como keycode).
pub const KC_UP: u8 = 128;
pub const KC_DOWN: u8 = 129;
pub const KC_LEFT: u8 = 130;
pub const KC_RIGHT: u8 = 131;
pub const KC_HOME: u8 = 132;
pub const KC_END: u8 = 133;
pub const KC_PAGEUP: u8 = 134;
pub const KC_PAGEDOWN: u8 = 135;
pub const KC_INSERT: u8 = 136;
pub const KC_DELETE: u8 = 137;
pub const KC_F1: u8 = 140; // F1..F12 = 140..=151

const FIFO_CAPACITY: usize = 64;

#[derive(Default)]
struct Registers {
    irq_enable: bool,
}

pub struct KeyboardMmio {
    base: u32,
    fifo: VecDeque<u32>,
    regs: Registers,
}

impl KeyboardMmio {
    pub fn new(base: u32) -> Self {
        Self {
            base,
            fifo: VecDeque::with_capacity(FIFO_CAPACITY),
            regs: Registers::default(),
        }
    }

    /// Llamado por el host (loop de eventos SDL) al capturar una tecla o
    /// texto. Si el FIFO está lleno se descarta el evento más nuevo (mejor
    /// perder una tecla que bloquear el emulador).
    pub fn push_event(&mut self, word: u32) {
        if self.fifo.len() < FIFO_CAPACITY {
            self.fifo.push_back(word);
        }
    }

    /// Si hay que levantar Cause.IP2 en este instante (nivel, no flanco):
    /// IRQ habilitada y FIFO con datos.
    pub fn has_pending_irq(&self) -> bool {
        self.regs.irq_enable && !self.fifo.is_empty()
    }

    #[inline]
    fn within(&self, paddr: u32) -> Option<u32> {
        let off = paddr.wrapping_sub(self.base);
        if off < 0x10 { Some(off) } else { None }
    }
}

impl Device for KeyboardMmio {
    fn range(&self) -> RangeInclusive<u32> {
        self.base..=self.base + 0x0F
    }

    fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        Ok(match off {
            // REG_DATA (0x00, u32): leer el byte más bajo ya hace pop.
            0x00 => self.fifo.pop_front().unwrap_or(0) as u8,
            0x01..=0x03 => 0, // el resto del word sólo se puede leer entero (ver read32)
            0x04 => (!self.fifo.is_empty()) as u8, // REG_STATUS bit0
            0x05..=0x07 => 0,
            0x08 => self.regs.irq_enable as u8, // REG_CTRL bit0
            _ => 0,
        })
    }

    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        if off == 0x08 {
            self.regs.irq_enable = value != 0;
        }
        Ok(())
    }

    fn read32(&mut self, paddr: u32) -> MemResult<u32> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        Ok(match off {
            0x00 => self.fifo.pop_front().unwrap_or(0), // REG_DATA: pop atómico de 32 bits
            0x04 => (!self.fifo.is_empty()) as u32,
            0x08 => self.regs.irq_enable as u32,
            _ => 0,
        })
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

    #[test]
    fn fifo_pop_via_read32_is_fifo_order() {
        let mut kbd = KeyboardMmio::new(0x1000);
        kbd.push_event(0x0000_0061); // 'a' pressed... (bit8 no seteado en este ejemplo simple)
        kbd.push_event(0x0000_0062); // 'b'
        assert_eq!(kbd.read32(0x1000).unwrap(), 0x61);
        assert_eq!(kbd.read32(0x1000).unwrap(), 0x62);
        assert_eq!(kbd.read32(0x1000).unwrap(), 0); // FIFO vacío -> 0
    }

    #[test]
    fn status_reflects_fifo_non_empty() {
        let mut kbd = KeyboardMmio::new(0x1000);
        assert_eq!(kbd.read32(0x1004).unwrap(), 0);
        kbd.push_event(0x41);
        assert_eq!(kbd.read32(0x1004).unwrap(), 1);
        kbd.read32(0x1000).unwrap(); // pop
        assert_eq!(kbd.read32(0x1004).unwrap(), 0);
    }

    #[test]
    fn irq_only_pending_when_enabled_and_data_available() {
        let mut kbd = KeyboardMmio::new(0x1000);
        kbd.push_event(0x41);
        assert!(!kbd.has_pending_irq()); // IRQ deshabilitada por default

        kbd.write8(0x1008, 1).unwrap(); // REG_CTRL: habilitar IRQ
        assert!(kbd.has_pending_irq());

        kbd.read32(0x1000).unwrap(); // vacía el FIFO
        assert!(!kbd.has_pending_irq());
    }

    #[test]
    fn fifo_drops_events_when_full_instead_of_blocking() {
        let mut kbd = KeyboardMmio::new(0x1000);
        for i in 0..FIFO_CAPACITY + 10 {
            kbd.push_event(i as u32);
        }
        assert_eq!(kbd.fifo.len(), FIFO_CAPACITY);
        assert_eq!(kbd.read32(0x1000).unwrap(), 0); // el evento más viejo sigue ahí
    }
}
