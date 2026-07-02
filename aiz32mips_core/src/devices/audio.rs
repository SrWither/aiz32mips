use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;
use std::collections::VecDeque;

// ───────────────────────────── audio.rs — MMIO de sonido ──────────────────
// Un solo canal, PCM mono s16 a SAMPLE_RATE fijo. Mismo patrón "submit +
// kick" que la GPU (ver gpu.rs::exec_command_buffer): el kernel copia
// samples a un buffer de staging propio del device (no RAM genérica —
// evita que este device tenga que resolver direcciones físicas del bus) y
// dispara KICK; acá se encolan en `queue`, que el loop principal del host
// (main.rs) va drenando hacia la salida de audio real (SDL AudioQueue).
// Sin mezcla, sin loop, sin volumen — lo mínimo para que un kernel pueda
// efectivamente hacer sonar algo.
pub const SAMPLE_RATE: u32 = 22050;

// Tope de un solo submit/kick: tiene que coincidir con
// kernel/audio.h::AUDIO_BUF_SAMPLES — sys_audio_submit trocea en el kernel
// si el proceso manda más que esto de una.
pub const STAGING_SAMPLES: usize = 1024;
const STAGING_BYTES: usize = STAGING_SAMPLES * 2;
const STAGING_OFF: u32 = 0x200; // mismo offset que STORAGE_BUF en storage.rs, mismo motivo

// Si nadie drena la cola (el harness headless de verify.rs no tiene salida
// de audio real) un kernel sometiendo audio sin parar no puede crecerla
// sin límite: tope arbitrario de ~4s a SAMPLE_RATE.
const QUEUE_CAP_SAMPLES: usize = (SAMPLE_RATE as usize) * 4;

pub struct AudioMmio {
    base: u32,
    staging: [u8; STAGING_BYTES],
    staging_len: u32, // samples válidos en `staging` para el próximo KICK
    queue: VecDeque<i16>,
    enabled: bool,
}

impl AudioMmio {
    pub fn new(base: u32) -> Self {
        Self {
            base,
            staging: [0u8; STAGING_BYTES],
            staging_len: 0,
            queue: VecDeque::new(),
            enabled: true,
        }
    }

    fn kick(&mut self) {
        if !self.enabled {
            return;
        }
        let n = (self.staging_len as usize).min(STAGING_SAMPLES);
        for i in 0..n {
            let lo = self.staging[i * 2] as u16;
            let hi = self.staging[i * 2 + 1] as u16;
            let sample = ((hi << 8) | lo) as i16;
            if self.queue.len() < QUEUE_CAP_SAMPLES {
                self.queue.push_back(sample);
            }
        }
    }

    /// La usa el host (main.rs) para sacar hasta `max` samples ya
    /// encolados y mandarlos a la salida de audio real.
    pub fn drain(&mut self, max: usize) -> Vec<i16> {
        let n = self.queue.len().min(max);
        self.queue.drain(..n).collect()
    }

    /// Samples todavía sin drenar — REG_AUDIO_STATUS lo expone tal cual
    /// para que el kernel pueda evitar acumular de más (ver sys_audio_status).
    pub fn pending(&self) -> u32 {
        self.queue.len() as u32
    }

    #[inline]
    fn within(&self, paddr: u32) -> Option<u32> {
        let off = paddr.wrapping_sub(self.base);
        if off < 0x1000 { Some(off) } else { None }
    }
}

impl Device for AudioMmio {
    fn range(&self) -> RangeInclusive<u32> {
        self.base..=self.base + 0xFFF
    }

    fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        if (STAGING_OFF..STAGING_OFF + STAGING_BYTES as u32).contains(&off) {
            return Ok(self.staging[(off - STAGING_OFF) as usize]);
        }
        Ok(match off {
            // REG_AUDIO_LEN (0x00, u32)
            0x00..=0x03 => ((self.staging_len >> ((off - 0x00) * 8)) & 0xFF) as u8,
            // REG_AUDIO_STATUS (0x08, u32): samples pendientes en la cola de reproducción
            0x08..=0x0B => ((self.pending() >> ((off - 0x08) * 8)) & 0xFF) as u8,
            0x0C => self.enabled as u8, // REG_AUDIO_CTRL
            _ => 0,
        })
    }

    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        if (STAGING_OFF..STAGING_OFF + STAGING_BYTES as u32).contains(&off) {
            self.staging[(off - STAGING_OFF) as usize] = value;
            return Ok(());
        }
        macro_rules! rmw32 {
            ($field:expr, $base:expr) => {{
                let shift = (off - $base) * 8;
                let mask = !(0xFFu32 << shift);
                $field = ($field & mask) | ((value as u32) << shift);
            }};
        }
        match off {
            0x00..=0x03 => rmw32!(self.staging_len, 0x00),
            0x04 => self.kick(), // REG_AUDIO_KICK: cualquier escritura dispara el encolado
            0x0C => {
                // REG_AUDIO_CTRL: bit0 = enable. Apagarlo corta YA (vacía la
                // cola) — lo usa audio_release_if_owner (trap.c) cuando el
                // proceso dueño muere, mismo criterio que gpu_release_if_owner.
                self.enabled = value != 0;
                if !self.enabled {
                    self.queue.clear();
                }
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

    fn push_samples(dev: &mut AudioMmio, base: u32, samples: &[i16]) {
        for (i, s) in samples.iter().enumerate() {
            let off = STAGING_OFF + (i as u32) * 2;
            dev.write8(base + off, (*s as u16 & 0xFF) as u8).unwrap();
            dev.write8(base + off + 1, ((*s as u16) >> 8) as u8).unwrap();
        }
        dev.write32(base + 0x00, samples.len() as u32).unwrap();
        dev.write8(base + 0x04, 1).unwrap(); // KICK
    }

    #[test]
    fn kick_enqueues_staged_samples_in_order() {
        let mut dev = AudioMmio::new(0x1000);
        push_samples(&mut dev, 0x1000, &[100, -200, 300]);
        assert_eq!(dev.pending(), 3);
        assert_eq!(dev.drain(10), vec![100, -200, 300]);
        assert_eq!(dev.pending(), 0);
    }

    #[test]
    fn status_reflects_pending_count_after_partial_drain() {
        let mut dev = AudioMmio::new(0x1000);
        push_samples(&mut dev, 0x1000, &[1, 2, 3, 4]);
        assert_eq!(dev.read32(0x1008).unwrap(), 4);
        dev.drain(2);
        assert_eq!(dev.read32(0x1008).unwrap(), 2);
    }

    #[test]
    fn disabling_ctrl_clears_the_queue_and_stops_future_kicks() {
        let mut dev = AudioMmio::new(0x1000);
        push_samples(&mut dev, 0x1000, &[1, 2, 3]);
        assert_eq!(dev.pending(), 3);

        dev.write8(0x1000 + 0x0C, 0).unwrap(); // CTRL=0: apaga y vacía
        assert_eq!(dev.pending(), 0);

        push_samples(&mut dev, 0x1000, &[9, 9, 9]);
        assert_eq!(dev.pending(), 0); // deshabilitado: KICK no encola nada

        dev.write8(0x1000 + 0x0C, 1).unwrap(); // re-habilita
        push_samples(&mut dev, 0x1000, &[5]);
        assert_eq!(dev.pending(), 1);
    }

    #[test]
    fn queue_caps_instead_of_growing_unbounded() {
        let mut dev = AudioMmio::new(0x1000);
        let one = [0i16; STAGING_SAMPLES];
        // más kicks que los que entrarían en QUEUE_CAP_SAMPLES
        for _ in 0..(QUEUE_CAP_SAMPLES / STAGING_SAMPLES + 5) {
            push_samples(&mut dev, 0x1000, &one);
        }
        assert!(dev.pending() as usize <= QUEUE_CAP_SAMPLES);
    }
}
