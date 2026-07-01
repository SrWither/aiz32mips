use sdl2::EventPump;
use sdl2::render::TextureCreator;
use sdl2::video::WindowContext;
use sdl2::{Sdl, pixels::PixelFormatEnum, rect::Rect, video::Window};

use crate::mmio_offsets::*;
use aiz32mips_core::devices::vram::SharedVram;
use aiz32mips_core::memory::MemoryBus;

pub struct SdlDisplay {
    #[allow(dead_code)]
    sdl: Sdl,
    event_pump: EventPump,
    canvas: sdl2::render::Canvas<Window>,
    tex_creator: TextureCreator<WindowContext>,
    vram: SharedVram,
    cur_w: u32,
    cur_h: u32,
    scale: u32,
}

impl SdlDisplay {
    pub fn new(initial_scale: u32, vram: SharedVram) -> anyhow::Result<Self> {
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
        // Un solo EventPump para toda la vida del programa: crear uno nuevo
        // en cada llamada (como hacía antes) era el cuello de botella real
        // del "1 fps" — se estaba haciendo polling de eventos SDL/X11 en
        // CADA instrucción de CPU emulada, millones de veces por segundo.
        let event_pump = sdl
            .event_pump()
            .map_err(|e| anyhow::anyhow!("SDL event pump error: {}", e))?;

        Ok(Self {
            sdl,
            event_pump,
            canvas,
            tex_creator,
            vram,
            cur_w: 0,
            cur_h: 0,
            scale: initial_scale.max(1),
        })
    }

    fn read_regs(bus: &mut MemoryBus) -> anyhow::Result<(u32, u32, u32)> {
        // Devuelve (w, h, fb_off): qué framebuffer mostrar lo decide la GPU
        // (DISPLAY_FB en STATUS), no el host.
        let w = bus
            .read16_virt(REG_FB_WIDTH)
            .map_err(|e| anyhow::anyhow!("{:?}", e))? as u32;
        let h = bus
            .read16_virt(REG_FB_HEIGHT)
            .map_err(|e| anyhow::anyhow!("{:?}", e))? as u32;
        let status = bus
            .read32_virt(REG_STATUS)
            .map_err(|e| anyhow::anyhow!("{:?}", e))?;
        let display_fb = (status >> 3) & 1;
        let fb_reg = if display_fb == 1 { REG_FB1_ADDR } else { REG_FB0_ADDR };
        let fb_off = bus
            .read32_virt(fb_reg)
            .map_err(|e| anyhow::anyhow!("{:?}", e))?;
        Ok((w, h, fb_off))
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
        for e in self.event_pump.poll_iter() {
            use sdl2::event::Event;
            match e {
                Event::Quit { .. } => return true,
                _ => {}
            }
        }
        false
    }

    pub fn present_from_bus(&mut self, bus: &mut MemoryBus) -> anyhow::Result<()> {
        let (w, h, fb_off) = Self::read_regs(bus)?;
        if w == 0 || h == 0 {
            return Ok(());
        }

        self.ensure_texture(w, h)?;

        let stride = w as usize * 4;
        let fb_off = fb_off as usize;
        let vram = self.vram.borrow();
        let src_len = (h as usize) * stride;
        if fb_off + src_len > vram.data.len() {
            return Ok(()); // configuración inválida, no reventamos el frame
        }
        let src = &vram.data[fb_off..fb_off + src_len];

        let mut tex = self
            .tex_creator
            .create_texture_streaming(PixelFormatEnum::ARGB8888, w, h)
            .map_err(|e| anyhow::anyhow!("SDL texture error: {}", e))?;

        // Copia en bloque (una sola pasada por línea) en vez de leer byte a
        // byte a través del bus, que era el cuello de botella real: 320x200
        // a 32bpp son 256000 accesos individuales por frame antes de esto.
        tex.with_lock(None, |buf: &mut [u8], pitch: usize| {
            for y in 0..h as usize {
                let src_line = &src[y * stride..y * stride + stride];
                let dst_line = &mut buf[y * pitch..y * pitch + stride];
                dst_line.copy_from_slice(src_line);
            }
        })
        .map_err(|e| anyhow::anyhow!("SDL lock error: {}", e))?;
        drop(vram);

        self.canvas.clear();
        let dst = Rect::new(0, 0, w * self.scale, h * self.scale);
        self.canvas
            .copy(&tex, None, dst)
            .map_err(|e| anyhow::anyhow!("SDL copy error: {}", e))?;
        self.canvas.present();
        Ok(())
    }
}
