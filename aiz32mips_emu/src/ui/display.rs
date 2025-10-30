use sdl2::render::TextureCreator;
use sdl2::video::WindowContext;
use sdl2::{Sdl, pixels::PixelFormatEnum, rect::Rect, video::Window};

use crate::mmio_offsets::*;
use aiz32mips_core::memory::MemoryBus;

pub struct SdlDisplay {
    sdl: Sdl,
    canvas: sdl2::render::Canvas<Window>,
    tex_creator: TextureCreator<WindowContext>,
    cur_w: u32,
    cur_h: u32,
    scale: u32,
}

impl SdlDisplay {
    pub fn new(initial_scale: u32) -> anyhow::Result<Self> {
        // sdl2::init() devuelve Result<_, String>, lo convertimos manualmente
        let sdl = sdl2::init().map_err(|e| anyhow::anyhow!("SDL init error: {}", e))?;
        let video = sdl
            .video()
            .map_err(|e| anyhow::anyhow!("SDL video error: {}", e))?;
        let window = video
            .window(
                "AIZ-32 | Framebuffer",
                320 * initial_scale,
                200 * initial_scale,
            )
            .position_centered()
            .opengl()
            .resizable()
            .build()
            .map_err(|e| anyhow::anyhow!("SDL window error: {}", e))?;
        let canvas = window
            .into_canvas()
            .present_vsync()
            .build()
            .map_err(|e| anyhow::anyhow!("SDL canvas error: {}", e))?;
        let tex_creator = canvas.texture_creator();

        Ok(Self {
            sdl,
            canvas,
            tex_creator,
            cur_w: 0,
            cur_h: 0,
            scale: initial_scale.max(1),
        })
    }

    fn read_regs(bus: &mut MemoryBus) -> anyhow::Result<(u32, u32, u32, u32, u32)> {
        // Devuelve (w, h, pitch_pixels, bpp, fb_off)
        let w = bus
            .read16_virt(REG_WIDTH)
            .map_err(|e| anyhow::anyhow!("{:?}", e))? as u32;
        let h = bus
            .read16_virt(REG_HEIGHT)
            .map_err(|e| anyhow::anyhow!("{:?}", e))? as u32;
        let pit = bus
            .read16_virt(REG_PITCH)
            .map_err(|e| anyhow::anyhow!("{:?}", e))? as u32;
        let bpp = bus
            .read8_virt(REG_BPP)
            .map_err(|e| anyhow::anyhow!("{:?}", e))? as u32;
        let fblo = bus
            .read32_virt(REG_FBADDR)
            .map_err(|e| anyhow::anyhow!("{:?}", e))? as u32;
        Ok((w, h, pit, bpp, fblo))
    }

    fn ensure_texture(&mut self, w: u32, h: u32) -> anyhow::Result<()> {
        if self.cur_w != w || self.cur_h != h {
            self.cur_w = w;
            self.cur_h = h;

            let win = self.canvas.window_mut();
            win.set_size(w * self.scale, h * self.scale)
                .map_err(|e| anyhow::anyhow!("SDL resize error: {}", e))?;
        }
        Ok(())
    }

    pub fn pump_events_quit(&mut self) -> bool {
        let mut pump = self.sdl.event_pump().unwrap();
        for e in pump.poll_iter() {
            use sdl2::event::Event;
            match e {
                Event::Quit { .. } => return true,
                _ => {}
            }
        }
        false
    }

    pub fn present_from_bus(&mut self, bus: &mut MemoryBus) -> anyhow::Result<()> {
        let (w, h, pitch_pixels, bpp, fb_off) = Self::read_regs(bus)?;
        if w == 0 || h == 0 || bpp != 32 {
            return Ok(());
        }

        self.ensure_texture(w, h)?;

        let stride = pitch_pixels * 4;

        let mut tex = self
            .tex_creator
            .create_texture_streaming(PixelFormatEnum::ARGB8888, w, h)
            .map_err(|e| anyhow::anyhow!("SDL texture error: {}", e))?;

        tex.with_lock(None, |buf: &mut [u8], pitch: usize| {
            for y in 0..h {
                let src_line_off = fb_off + y * stride;
                let dst = &mut buf[(y as usize) * pitch..(y as usize) * pitch + (w as usize) * 4];

                for x in 0..(w * 4) {
                    let byte = bus.read8_virt(0x1000_0000 + src_line_off + x).unwrap_or(0);
                    dst[x as usize] = byte;
                }
            }
        })
        .map_err(|e| anyhow::anyhow!("SDL lock error: {}", e))?;

        self.canvas.clear();
        let dst = Rect::new(0, 0, w * self.scale, h * self.scale);
        self.canvas
            .copy(&tex, None, dst)
            .map_err(|e| anyhow::anyhow!("SDL copy error: {}", e))?;
        self.canvas.present();
        Ok(())
    }
}
