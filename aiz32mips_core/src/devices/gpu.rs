use crate::devices::vram::GpuVram;
use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;

pub struct GpuMmio {
    base: u32,
    regs: Registers,
    fifo: Fifo,
    vram_ptr: *mut GpuVram,
}

#[derive(Default)]
struct Registers {
    width: u16,
    height: u16,
    pitch: u16,
    bpp: u8,
    fb_addr: u32,
    status: u32,
    font_addr: u32,
    font_w: u8,
    font_h: u8,
    palette_addr: u32,
}

struct Fifo {
    buf: [u16; 256],
    head: usize,
}
impl Fifo {
    fn new() -> Self {
        Self {
            buf: [0; 256],
            head: 0,
        }
    }
    fn reset(&mut self) {
        self.head = 0;
    }
    fn push(&mut self, v: u16) {
        if self.head < self.buf.len() {
            self.buf[self.head] = v;
            self.head += 1;
        }
    }
    fn read_u16(&mut self) -> u16 {
        let i = self.head.min(self.buf.len());
        if i == 0 {
            0
        } else {
            self.head -= 1;
            self.buf[self.head]
        }
    }
    fn pop(&mut self) -> u16 {
        if self.head == 0 {
            0
        } else {
            let v = self.buf[0];
            self.buf.copy_within(1..self.head, 0);
            self.head -= 1;
            v
        }
    }
    fn pop_u32(&mut self) -> u32 {
        (self.pop() as u32) | ((self.pop() as u32) << 16)
    }
}

impl GpuMmio {
    pub fn new(base: u32, vram: &mut GpuVram) -> Self {
        Self {
            base,
            regs: Registers::default(),
            fifo: Fifo::new(),
            vram_ptr: vram as *mut _,
        }
    }

    #[inline]
    fn pitch_bytes(&self) -> usize {
        match self.regs.bpp {
            32 => (self.regs.pitch as usize) * 4,
            8 => self.regs.pitch as usize,
            _ => self.regs.pitch as usize, // simple por ahora
        }
    }

    #[inline]
    fn clamp_to_fb(&self, x: i32, y: i32) -> Option<(usize, usize)> {
        let w = self.regs.width as i32;
        let h = self.regs.height as i32;
        if x < 0 || y < 0 || x >= w || y >= h {
            return None;
        }
        Some((x as usize, y as usize))
    }

    fn put_pixel_32(&mut self, x: i32, y: i32, color: u32) {
        if self.regs.bpp != 32 {
            return;
        }
        let (x, y) = match self.clamp_to_fb(x, y) {
            Some(v) => v,
            None => return,
        };
        let fb_off = self.regs.fb_addr as usize;
        let stride = self.pitch_bytes();
        let base = fb_off + y * stride + x * 4;

        let vram = unsafe { &mut *self.vram_ptr };
        vram.slice_mut()[base..base + 4].copy_from_slice(&color.to_le_bytes());
    }

    fn cmd_fillrect(&mut self, x: u16, y: u16, w: u16, h: u16, color: u32) {
        self.set_busy(true);
        if self.regs.bpp != 32 {
            self.set_busy(false);
            return;
        }

        let fb_off = self.regs.fb_addr as usize;
        let stride = self.pitch_bytes();
        let max_w = self.regs.width as usize;
        let max_h = self.regs.height as usize;

        let vx = x as usize;
        let vy = y as usize;
        let vw = w as usize;
        let vh = h as usize;

        if vx >= max_w || vy >= max_h {
            self.set_busy(false);
            return;
        }

        let rw = vw.min(max_w - vx);
        let rh = vh.min(max_h - vy);

        let color4 = color.to_le_bytes();
        let vram = unsafe { &mut *self.vram_ptr };

        for j in 0..rh {
            // posicionarnos al inicio de la fila en FB
            let ptr = fb_off + (vy + j) * stride + vx * 4;
            let line = &mut vram.slice_mut()[ptr..ptr + rw * 4];

            // escribir 4 bytes por pixel (sin loop anidado para off-by-one claro)
            // still loop, pero con slice ya tomada:
            for px in 0..rw {
                let off = px * 4;
                line[off..off + 4].copy_from_slice(&color4);
            }
        }
        self.set_busy(false);
    }

    fn cmd_grad_y(&mut self, top: u32, bottom: u32) {
        self.set_busy(true);
        if self.regs.bpp != 32 {
            self.set_busy(false);
            return;
        }

        let fb_off = self.regs.fb_addr as usize;
        let w = self.regs.width as usize;
        let h = self.regs.height as usize;
        let stride = self.pitch_bytes();

        let vram = unsafe { &mut *self.vram_ptr };
        for y in 0..h {
            let c = Self::lerp(top, bottom, y, h.saturating_sub(1).max(1));
            let mut p = fb_off + y * stride;
            for _ in 0..w {
                vram.slice_mut()[p..p + 4].copy_from_slice(&c.to_le_bytes());
                p += 4;
            }
        }
        self.set_busy(false);
    }

    fn cmd_grad_xy(&mut self, c00: u32, c10: u32, c01: u32, c11: u32) {
        self.set_busy(true);
        if self.regs.bpp != 32 {
            self.set_busy(false);
            return;
        }

        let fb_off = self.regs.fb_addr as usize;
        let w = self.regs.width as usize;
        let h = self.regs.height as usize;
        let stride = self.pitch_bytes();

        let vram = unsafe { &mut *self.vram_ptr };

        for y in 0..h {
            let left = Self::lerp(c00, c01, y, h.saturating_sub(1).max(1));
            let right = Self::lerp(c10, c11, y, h.saturating_sub(1).max(1));

            let mut p = fb_off + y * stride;
            for x in 0..w {
                let c = Self::lerp(left, right, x, w.saturating_sub(1).max(1));
                vram.slice_mut()[p..p + 4].copy_from_slice(&c.to_le_bytes());
                p += 4;
            }
        }

        self.set_busy(false);
    }

    fn cmd_rect_outline(&mut self, x: u16, y: u16, w: u16, h: u16, color: u32) {
        self.set_busy(true);
        if self.regs.bpp != 32 {
            self.set_busy(false);
            return;
        }

        let x0 = x as i32;
        let y0 = y as i32;
        let x1 = x0 + (w as i32) - 1;
        let y1 = y0 + (h as i32) - 1;

        // top/bottom
        for xx in x0..=x1 {
            self.put_pixel_32(xx, y0, color);
            self.put_pixel_32(xx, y1, color);
        }
        // left/right
        for yy in y0..=y1 {
            self.put_pixel_32(x0, yy, color);
            self.put_pixel_32(x1, yy, color);
        }
        self.set_busy(false);
    }

    fn cmd_line(&mut self, x0: u16, y0: u16, x1: u16, y1: u16, color: u32) {
        self.set_busy(true);
        if self.regs.bpp != 32 {
            self.set_busy(false);
            return;
        }

        let mut x0 = x0 as i32;
        let mut y0 = y0 as i32;
        let x1 = x1 as i32;
        let y1 = y1 as i32;

        // Bresenham
        let dx = (x1 - x0).abs();
        let sx = if x0 < x1 { 1 } else { -1 };
        let dy = -(y1 - y0).abs();
        let sy = if y0 < y1 { 1 } else { -1 };
        let mut err = dx + dy;

        loop {
            self.put_pixel_32(x0, y0, color);
            if x0 == x1 && y0 == y1 {
                break;
            }
            let e2 = 2 * err;
            if e2 >= dy {
                err += dy;
                x0 += sx;
            }
            if e2 <= dx {
                err += dx;
                y0 += sy;
            }
        }

        self.set_busy(false);
    }

    fn cmd_blit(&mut self, src_addr: u32, src_w: u16, src_h: u16, dst_x: u16, dst_y: u16) {
        self.set_busy(true);
        if self.regs.bpp != 32 {
            self.set_busy(false);
            return;
        }

        let fb_off = self.regs.fb_addr as usize;
        let stride = self.pitch_bytes();
        let dst_w = self.regs.width as usize;
        let dst_h = self.regs.height as usize;

        let sw = src_w as usize;
        let sh = src_h as usize;
        let dx0 = dst_x as usize;
        let dy0 = dst_y as usize;

        // recorte
        if dx0 >= dst_w || dy0 >= dst_h {
            self.set_busy(false);
            return;
        }
        let copy_w = sw.min(dst_w - dx0);
        let copy_h = sh.min(dst_h - dy0);

        let src_base = src_addr as usize;

        let vram_ptr = self.vram_ptr;
        let vram = unsafe { &mut *vram_ptr };

        for j in 0..copy_h {
            let mut tmp = Vec::with_capacity(copy_w * 4);
            {
                let src = &vram.slice()[src_base + j * sw * 4..src_base + j * sw * 4 + copy_w * 4];
                tmp.extend_from_slice(src);
            }

            {
                let dst_off = fb_off + (dy0 + j) * stride + dx0 * 4;
                let dst = &mut vram.slice_mut()[dst_off..dst_off + copy_w * 4];
                dst.copy_from_slice(&tmp);
            }
        }

        self.set_busy(false);
    }

    #[inline]
    fn within(&self, paddr: u32) -> Option<u32> {
        let off = paddr.wrapping_sub(self.base);
        if off < 0x100 { Some(off) } else { None }
    }

    fn set_busy(&mut self, busy: bool) {
        if busy {
            self.regs.status |= 1
        } else {
            self.regs.status &= !1
        }
    }

    fn cmd_clear(&mut self, color: u32) {
        self.set_busy(true);

        let bpp = self.regs.bpp;
        let fb_off = self.regs.fb_addr as usize;
        let w = self.regs.width as usize;
        let h = self.regs.height as usize;

        let vram_ptr = self.vram_ptr;
        let vram = unsafe { &mut *vram_ptr };

        match bpp {
            32 => {
                let mut p = fb_off;
                for _y in 0..h {
                    for _x in 0..w {
                        vram.slice_mut()[p..p + 4].copy_from_slice(&color.to_le_bytes());
                        p += 4;
                    }
                }
            }
            8 | 4 | 2 => {
                let mut p = fb_off;
                let val = (color & 0xFF) as u8;
                for _y in 0..h {
                    for _x in 0..w {
                        vram.slice_mut()[p] = val;
                        p += 1;
                    }
                }
            }
            _ => {}
        }

        self.set_busy(false);
    }

    fn lerp(a: u32, b: u32, t_num: usize, t_den: usize) -> u32 {
        // ARGB8888: linealizamos por canal
        let ar = ((a >> 24) & 0xFF) as usize;
        let ag = ((a >> 16) & 0xFF) as usize;
        let ab = ((a >> 8) & 0xFF) as usize;
        let aa = ((a >> 0) & 0xFF) as usize;
        let br = ((b >> 24) & 0xFF) as usize;
        let bg = ((b >> 16) & 0xFF) as usize;
        let bb = ((b >> 8) & 0xFF) as usize;
        let ba = ((b >> 0) & 0xFF) as usize;
        let lr = (ar * (t_den - t_num) + br * t_num) / t_den;
        let lg = (ag * (t_den - t_num) + bg * t_num) / t_den;
        let lb = (ab * (t_den - t_num) + bb * t_num) / t_den;
        let la = (aa * (t_den - t_num) + ba * t_num) / t_den;
        ((lr as u32) << 24) | ((lg as u32) << 16) | ((lb as u32) << 8) | (la as u32)
    }

    fn cmd_grad_x(&mut self, left: u32, right: u32) {
        self.set_busy(true);
        let fb_off = self.regs.fb_addr as usize;
        let w = self.regs.width as usize;
        let h = self.regs.height as usize;

        let vram_ptr = self.vram_ptr;
        let vram = unsafe { &mut *vram_ptr };

        if self.regs.bpp == 32 {
            for y in 0..h {
                let mut p = fb_off + y * (self.regs.pitch as usize * 4);
                for x in 0..w {
                    let c = Self::lerp(left, right, x, w.saturating_sub(1).max(1));
                    vram.slice_mut()[p..p + 4].copy_from_slice(&c.to_le_bytes());
                    p += 4;
                }
            }
        }
        self.set_busy(false);
    }

    fn cmd_putchar(&mut self, x: u16, y: u16, ch: u16, fg: u32, bg: u32) {
        self.set_busy(true);
        let fw = self.regs.font_w as usize;
        let fh = self.regs.font_h as usize;
        if fw == 0 || fh == 0 {
            self.set_busy(false);
            return;
        }

        let font_base = self.regs.font_addr as usize;
        let glyph_bytes_per_row = ((fw + 7) / 8) as usize;
        let glyph_size = glyph_bytes_per_row * fh;
        let glyph_off = font_base + (ch as usize) * glyph_size;

        let w = self.regs.width as usize;
        let bpp = self.regs.bpp;

        let vram_ptr = self.vram_ptr;
        let vram = unsafe { &mut *vram_ptr };

        let pitch_bytes = match bpp {
            32 => self.regs.pitch as usize * 4,
            _ => self.regs.pitch as usize,
        };
        let fb_off = self.regs.fb_addr as usize;

        for j in 0..fh {
            let row_byte = vram.slice()[glyph_off + j * glyph_bytes_per_row];
            for i in 0..fw {
                let bit = (row_byte >> (7 - (i & 7))) & 1;
                let px = x as usize + i;
                let py = y as usize + j;
                if px >= w {
                    continue;
                }
                let base = fb_off
                    + py * pitch_bytes
                    + match bpp {
                        32 => px * 4,
                        _ => px,
                    };
                match bpp {
                    32 => {
                        let color = if bit != 0 { fg } else { bg };
                        vram.slice_mut()[base..base + 4].copy_from_slice(&color.to_le_bytes());
                    }
                    8 => {
                        // escribir índice 0 o 1 (usa tu paleta); simplificado:
                        vram.slice_mut()[base] = if bit != 0 {
                            (fg & 0xFF) as u8
                        } else {
                            (bg & 0xFF) as u8
                        };
                    }
                    _ => {}
                }
            }
        }
        self.set_busy(false);
    }

    fn cmd_puts(&mut self, x: u16, y: u16, len: u16, fg: u32, bg: u32) {
        let mut cx = x;
        for _ in 0..len {
            let ch = self.fifo.pop() as u16;
            self.cmd_putchar(cx, y, ch, fg, bg);
            cx = cx.wrapping_add(self.regs.font_w as u16);
        }
    }

    fn cmd_blit_tilemap(
        &mut self,
        tilemap_addr: u32,
        map_w: u16,
        map_h: u16,
        tile_w: u8,
        tile_h: u8,
        tileset_addr: u32,
        bpp: u8,
    ) {
        self.set_busy(true);
        let _ = (
            tilemap_addr,
            map_w,
            map_h,
            tile_w,
            tile_h,
            tileset_addr,
            bpp,
        );
        self.set_busy(false);
    }

    fn exec_cmd(&mut self, cmd: u16) {
        match cmd {
            0x0001 => {
                // CLEAR
                let color = self.fifo.pop_u32();
                self.cmd_clear(color);
            }
            0x0002 => {
                // GRAD_X
                let left = self.fifo.pop_u32();
                let right = self.fifo.pop_u32();
                self.cmd_grad_x(left, right);
            }
            0x0003 => {
                // PUTCHAR
                let x = self.fifo.pop();
                let y = self.fifo.pop();
                let ch = self.fifo.pop();
                let fg = self.fifo.pop_u32();
                let bg = self.fifo.pop_u32();
                self.cmd_putchar(x, y, ch, fg, bg);
            }
            0x0004 => {
                // PUTS
                let x = self.fifo.pop();
                let y = self.fifo.pop();
                let len = self.fifo.pop();
                let fg = self.fifo.pop_u32();
                let bg = self.fifo.pop_u32();
                self.cmd_puts(x, y, len, fg, bg);
            }
            0x0006 => {
                // FILLRECT: x,y,w,h,color
                let x = self.fifo.pop();
                let y = self.fifo.pop();
                let w = self.fifo.pop();
                let h = self.fifo.pop();
                let color = self.fifo.pop_u32();
                self.cmd_fillrect(x, y, w, h, color);
            }
            0x0007 => {
                // GRAD_Y: top, bottom
                let top = self.fifo.pop_u32();
                let bot = self.fifo.pop_u32();
                self.cmd_grad_y(top, bot);
            }
            0x0008 => {
                // RECT_OUTLINE: x,y,w,h,color
                let x = self.fifo.pop();
                let y = self.fifo.pop();
                let w = self.fifo.pop();
                let h = self.fifo.pop();
                let color = self.fifo.pop_u32();
                self.cmd_rect_outline(x, y, w, h, color);
            }
            0x0009 => {
                // LINE: x0,y0,x1,y1,color
                let x0 = self.fifo.pop();
                let y0 = self.fifo.pop();
                let x1 = self.fifo.pop();
                let y1 = self.fifo.pop();
                let color = self.fifo.pop_u32();
                self.cmd_line(x0, y0, x1, y1, color);
            }
            0x000A => {
                // BLIT: src_addr, src_w, src_h, dst_x, dst_y  (ARGB8888 contiguo)
                let src_addr = self.fifo.pop_u32();
                let src_w = self.fifo.pop();
                let src_h = self.fifo.pop();
                let dst_x = self.fifo.pop();
                let dst_y = self.fifo.pop();
                self.cmd_blit(src_addr, src_w, src_h, dst_x, dst_y);
            }
            0x0005 => {
                // BLIT_TILEMAP - stub que ya tenías
                let tm = self.fifo.pop_u32();
                let mw = self.fifo.pop();
                let mh = self.fifo.pop();
                let tw = self.fifo.pop() as u8;
                let th = self.fifo.pop() as u8;
                let ts = self.fifo.pop_u32();
                let bpp = self.regs.bpp;
                self.cmd_blit_tilemap(tm, mw, mh, tw, th, ts, bpp);
            }
            0x000B => {
                // GRAD_XY: c00, c10, c01, c11
                let c00 = self.fifo.pop_u32(); // top-left
                let c10 = self.fifo.pop_u32(); // top-right
                let c01 = self.fifo.pop_u32(); // bottom-left
                let c11 = self.fifo.pop_u32(); // bottom-right
                self.cmd_grad_xy(c00, c10, c01, c11);
            }
            _ => { /* no-op */ }
        }
        self.fifo.reset();
    }
}

impl Device for GpuMmio {
    fn range(&self) -> RangeInclusive<u32> {
        self.base..=self.base + 0xFF
    }

    fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        self.within(paddr)
            .map_or(Err(MemoryError::Unmapped(paddr)), |off| {
                Ok(match off {
                    0x00 => (self.regs.width & 0xFF) as u8,
                    0x01 => (self.regs.width >> 8) as u8,
                    0x02 => (self.regs.height & 0xFF) as u8,
                    0x03 => (self.regs.height >> 8) as u8,
                    0x04 => (self.regs.pitch & 0xFF) as u8,
                    0x05 => (self.regs.pitch >> 8) as u8,
                    0x06 => self.regs.bpp,
                    0x0C..=0x0F => ((self.regs.status >> ((off - 0x0C) * 8)) & 0xFF) as u8,
                    0x08..=0x0B => ((self.regs.fb_addr >> ((off - 0x08) * 8)) & 0xFF) as u8,
                    0x20..=0x23 => ((self.regs.font_addr >> ((off - 0x20) * 8)) & 0xFF) as u8,
                    0x24 => self.regs.font_w,
                    0x25 => self.regs.font_h,
                    0x28..=0x2B => ((self.regs.palette_addr >> ((off - 0x28) * 8)) & 0xFF) as u8,
                    _ => 0,
                })
            })
    }

    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };

        println!("GPU write8 off=0x{:02X} val=0x{:02X}", off, value);

        match off {
            0x00 => self.regs.width = (self.regs.width & 0xFF00) | value as u16,
            0x01 => self.regs.width = (self.regs.width & 0x00FF) | ((value as u16) << 8),
            0x02 => self.regs.height = (self.regs.height & 0xFF00) | value as u16,
            0x03 => self.regs.height = (self.regs.height & 0x00FF) | ((value as u16) << 8),
            0x04 => self.regs.pitch = (self.regs.pitch & 0xFF00) | value as u16,
            0x05 => self.regs.pitch = (self.regs.pitch & 0x00FF) | ((value as u16) << 8),
            0x06 => self.regs.bpp = value,
            0x08..=0x0B => {
                let shift = (off - 0x08) * 8;
                let mask = !(0xFFu32 << shift);
                self.regs.fb_addr = (self.regs.fb_addr & mask) | ((value as u32) << shift);
            }
            0x10..=0x11 => {
                // REG_CMD (16-bit), escribir lo ejecuta
                static mut CMD_TMP: u16 = 0;
                let lohi = off - 0x10;
                unsafe {
                    if lohi == 0 {
                        CMD_TMP = (CMD_TMP & 0xFF00) | value as u16;
                    } else {
                        CMD_TMP = (CMD_TMP & 0x00FF) | ((value as u16) << 8);
                        self.exec_cmd(CMD_TMP);
                        CMD_TMP = 0;
                    }
                }
            }
            0x12..=0x13 => {
                // REG_PARAM (16-bit FIFO) – cada par de bytes pushea un u16
                static mut TMP: u16 = 0;
                let lohi = off - 0x12;
                unsafe {
                    if lohi == 0 {
                        TMP = (TMP & 0xFF00) | value as u16;
                    } else {
                        TMP = (TMP & 0x00FF) | ((value as u16) << 8);
                        self.fifo.push(TMP);
                        TMP = 0;
                    }
                }
            }
            0x20..=0x23 => {
                let shift = (off - 0x20) * 8;
                let mask = !(0xFFu32 << shift);
                self.regs.font_addr = (self.regs.font_addr & mask) | ((value as u32) << shift);
            }
            0x24 => self.regs.font_w = value,
            0x25 => self.regs.font_h = value,
            0x28..=0x2B => {
                let shift = (off - 0x28) * 8;
                let mask = !(0xFFu32 << shift);
                self.regs.palette_addr =
                    (self.regs.palette_addr & mask) | ((value as u32) << shift);
            }
            _ => {}
        }
        Ok(())
    }
}
