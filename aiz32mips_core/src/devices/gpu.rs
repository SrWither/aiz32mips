use crate::devices::vram::SharedVram;
use crate::memory::{Device, MemResult, MemoryError};
use core::ops::RangeInclusive;

// ───────────────────────────── Layout de VRAM por defecto ─────────────────
// La GPU no impone este layout (todo es vía registros), pero son los
// offsets que usa `aiz32mips_emu` al bootear y que expone la lib en C.
pub const VRAM_FB0: u32 = 0x000000;
pub const VRAM_FB1: u32 = 0x040000;
pub const VRAM_ZBUF: u32 = 0x080000;
pub const VRAM_FONT: u32 = 0x0C0000;
pub const VRAM_TEXT: u32 = 0x0C1000;
pub const VRAM_PALETTE: u32 = 0x0C2000;
pub const VRAM_CMDBUF: u32 = 0x0C3000;
pub const VRAM_HEAP: u32 = 0x0D3000;

// ───────────────────────────── Opcodes del command buffer ─────────────────
// Cada comando es una lista de words u32 en VRAM; el primero es el opcode.
// El CPU arma la lista con SW normales y dispara CMD_KICK.
pub const OP_NOP: u32 = 0;
pub const OP_CLEAR: u32 = 1;
pub const OP_CLEAR_Z: u32 = 2;
pub const OP_SET_CLIP: u32 = 3;
pub const OP_SET_BLEND: u32 = 4;
pub const OP_FLIP: u32 = 5;
pub const OP_FILLRECT: u32 = 6;
pub const OP_RECT_OUTLINE: u32 = 7;
pub const OP_LINE: u32 = 8;
pub const OP_CIRCLE: u32 = 9;
pub const OP_TRIANGLE2D: u32 = 10;
pub const OP_GRAD_X: u32 = 11;
pub const OP_GRAD_Y: u32 = 12;
pub const OP_GRAD_XY: u32 = 13;
pub const OP_PUTCHAR: u32 = 14;
pub const OP_PUTS: u32 = 15;
pub const OP_BLIT: u32 = 16;
pub const OP_TRIANGLE3D: u32 = 17;

const BLIT_KEY: u32 = 1 << 0;
const BLIT_ALPHA: u32 = 1 << 1;
const BLIT_FLIP_X: u32 = 1 << 2;
const BLIT_FLIP_Y: u32 = 1 << 3;

const TRI_DEPTH_TEST: u32 = 1 << 0;
const TRI_DEPTH_WRITE: u32 = 1 << 1;
const TRI_GOURAUD: u32 = 1 << 2;
const TRI_TEXTURED: u32 = 1 << 3;
const TRI_BLEND: u32 = 1 << 4;

const DEFAULT_PALETTE: [u32; 16] = [
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA, 0xFFAA0000, 0xFFAA00AA, 0xFFAA5500,
    0xFFAAAAAA, 0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF, 0xFFFF5555, 0xFFFF55FF,
    0xFFFFFF55, 0xFFFFFFFF,
];

#[derive(Clone, Copy, PartialEq)]
enum BlendMode {
    Opaque,
    Alpha,
    Additive,
}

impl BlendMode {
    fn from_u32(v: u32) -> Self {
        match v {
            1 => BlendMode::Alpha,
            2 => BlendMode::Additive,
            _ => BlendMode::Opaque,
        }
    }
}

#[derive(Default)]
struct Registers {
    fb_width: u16,
    fb_height: u16,
    fb0_addr: u32,
    fb1_addr: u32,
    zbuf_addr: u32,
    draw_fb: u8,
    display_fb: u8,
    vblank: bool,
    cmd_addr: u32,
    cmd_len: u32,
    font_addr: u32,
    font_w: u8,
    font_h: u8,
    text_addr: u32,
    text_cols: u16,
    text_rows: u16,
    text_palette_addr: u32,
    text_enable: bool,
    clip_x0: i32,
    clip_y0: i32,
    clip_x1: i32,
    clip_y1: i32,
    blend_mode: u32,
}

pub struct GpuMmio {
    base: u32,
    regs: Registers,
    vram: SharedVram,
}

// ───────────────────────────── Helpers de bajo nivel ───────────────────────
// Funciones libres (no métodos) para poder operar sobre un &mut [u8] ya
// prestado una sola vez por comando, sin pedir el RefCell de vuelta por
// cada píxel.

#[inline]
fn blend_pixel(dst: u32, src: u32, mode: BlendMode) -> u32 {
    match mode {
        BlendMode::Opaque => src,
        BlendMode::Alpha => {
            let sa = (src >> 24) & 0xFF;
            if sa == 0 {
                return dst;
            }
            if sa >= 255 {
                return src;
            }
            let lerp = |s: u32, d: u32| -> u32 { (s * sa + d * (255 - sa)) / 255 };
            let (sr, sg, sb) = ((src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF);
            let (dr, dg, db) = ((dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF);
            0xFF00_0000 | (lerp(sr, dr) << 16) | (lerp(sg, dg) << 8) | lerp(sb, db)
        }
        BlendMode::Additive => {
            let (sr, sg, sb) = ((src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF);
            let (dr, dg, db) = ((dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF);
            0xFF00_0000
                | ((sr + dr).min(255) << 16)
                | ((sg + dg).min(255) << 8)
                | (sb + db).min(255)
        }
    }
}

#[inline]
fn lerp_channel(a: u32, b: u32, t_num: usize, t_den: usize) -> u32 {
    let an = ((a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
    let bn = ((b >> 24) & 0xFF, (b >> 16) & 0xFF, (b >> 8) & 0xFF, b & 0xFF);
    let l = |x: u32, y: u32| (x * (t_den - t_num) as u32 + y * t_num as u32) / t_den as u32;
    (l(an.0, bn.0) << 24) | (l(an.1, bn.1) << 16) | (l(an.2, bn.2) << 8) | l(an.3, bn.3)
}

#[inline]
fn lerp3_color(a: u32, b: u32, c: u32, w0: f64, w1: f64, w2: f64) -> u32 {
    let ch = |shift: u32| -> u32 {
        let av = ((a >> shift) & 0xFF) as f64;
        let bv = ((b >> shift) & 0xFF) as f64;
        let cv = ((c >> shift) & 0xFF) as f64;
        (av * w0 + bv * w1 + cv * w2).round().clamp(0.0, 255.0) as u32
    };
    (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0)
}

#[inline]
fn modulate(vcolor: u32, texel: u32) -> u32 {
    let vr = (vcolor >> 16) & 0xFF;
    let vg = (vcolor >> 8) & 0xFF;
    let vb = vcolor & 0xFF;
    let tr = (texel >> 16) & 0xFF;
    let tg = (texel >> 8) & 0xFF;
    let tb = texel & 0xFF;
    let ta = (texel >> 24) & 0xFF;
    (ta << 24) | (((vr * tr) / 255) << 16) | (((vg * tg) / 255) << 8) | ((vb * tb) / 255)
}

#[inline]
fn read_u32(data: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(data[off..off + 4].try_into().unwrap())
}

#[inline]
fn write_pixel(data: &mut [u8], off: usize, color: u32) {
    data[off..off + 4].copy_from_slice(&color.to_le_bytes());
}

type Clip = (i32, i32, i32, i32);

#[inline]
fn put_pixel(data: &mut [u8], fb: usize, stride: usize, clip: Clip, x: i32, y: i32, color: u32, blend: BlendMode) {
    if x < clip.0 || y < clip.1 || x > clip.2 || y > clip.3 {
        return;
    }
    let off = fb + (y as usize) * stride + (x as usize) * 4;
    if off + 4 > data.len() {
        return;
    }
    let out = if blend == BlendMode::Opaque {
        color
    } else {
        blend_pixel(read_u32(data, off), color, blend)
    };
    write_pixel(data, off, out);
}

fn fill_rect_raw(data: &mut [u8], fb: usize, stride: usize, clip: Clip, x: i32, y: i32, w: i32, h: i32, color: u32, blend: BlendMode) {
    let x0 = x.max(clip.0);
    let y0 = y.max(clip.1);
    let x1 = (x + w - 1).min(clip.2);
    let y1 = (y + h - 1).min(clip.3);
    for yy in y0..=y1.max(y0 - 1) {
        for xx in x0..=x1.max(x0 - 1) {
            put_pixel(data, fb, stride, clip, xx, yy, color, blend);
        }
    }
}

fn line_raw(data: &mut [u8], fb: usize, stride: usize, clip: Clip, x0: i32, y0: i32, x1: i32, y1: i32, color: u32, blend: BlendMode) {
    let mut x0 = x0;
    let mut y0 = y0;
    let dx = (x1 - x0).abs();
    let sx = if x0 < x1 { 1 } else { -1 };
    let dy = -(y1 - y0).abs();
    let sy = if y0 < y1 { 1 } else { -1 };
    let mut err = dx + dy;
    loop {
        put_pixel(data, fb, stride, clip, x0, y0, color, blend);
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
}

fn circle_raw(data: &mut [u8], fb: usize, stride: usize, clip: Clip, cx: i32, cy: i32, r: i32, color: u32, filled: bool, blend: BlendMode) {
    let mut x = r;
    let mut y = 0;
    let mut err = 0;
    while x >= y {
        if filled {
            line_raw(data, fb, stride, clip, cx - x, cy + y, cx + x, cy + y, color, blend);
            line_raw(data, fb, stride, clip, cx - x, cy - y, cx + x, cy - y, color, blend);
            line_raw(data, fb, stride, clip, cx - y, cy + x, cx + y, cy + x, color, blend);
            line_raw(data, fb, stride, clip, cx - y, cy - x, cx + y, cy - x, color, blend);
        } else {
            for (px, py) in [
                (cx + x, cy + y), (cx - x, cy + y), (cx + x, cy - y), (cx - x, cy - y),
                (cx + y, cy + x), (cx - y, cy + x), (cx + y, cy - x), (cx - y, cy - x),
            ] {
                put_pixel(data, fb, stride, clip, px, py, color, blend);
            }
        }
        y += 1;
        if err <= 0 {
            err += 2 * y + 1;
        }
        if err > 0 {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

#[inline]
fn edge(ax: i32, ay: i32, bx: i32, by: i32, px: i32, py: i32) -> i64 {
    (bx as i64 - ax as i64) * (py as i64 - ay as i64) - (by as i64 - ay as i64) * (px as i64 - ax as i64)
}

fn fill_triangle2d_raw(data: &mut [u8], fb: usize, stride: usize, clip: Clip, v: [(i32, i32); 3], color: u32, blend: BlendMode) {
    let min_x = v[0].0.min(v[1].0).min(v[2].0).max(clip.0);
    let max_x = v[0].0.max(v[1].0).max(v[2].0).min(clip.2);
    let min_y = v[0].1.min(v[1].1).min(v[2].1).max(clip.1);
    let max_y = v[0].1.max(v[1].1).max(v[2].1).min(clip.3);
    if min_x > max_x || min_y > max_y {
        return;
    }
    let area = edge(v[0].0, v[0].1, v[1].0, v[1].1, v[2].0, v[2].1);
    if area == 0 {
        return;
    }
    for y in min_y..=max_y {
        for x in min_x..=max_x {
            let w0 = edge(v[1].0, v[1].1, v[2].0, v[2].1, x, y);
            let w1 = edge(v[2].0, v[2].1, v[0].0, v[0].1, x, y);
            let w2 = edge(v[0].0, v[0].1, v[1].0, v[1].1, x, y);
            let inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if inside {
                put_pixel(data, fb, stride, clip, x, y, color, blend);
            }
        }
    }
}

/// Vértice ya transformado a espacio de pantalla (lo arma el CPU/software).
#[derive(Clone, Copy)]
struct Vertex3 {
    x: i32,
    y: i32,
    z: u32,
    color: u32,
    u: i32,
    v: i32,
}

#[allow(clippy::too_many_arguments)]
fn fill_triangle3d_raw(
    data: &mut [u8],
    fb: usize,
    stride: usize,
    clip: Clip,
    zbuf: Option<usize>,
    fb_w: usize,
    v: [Vertex3; 3],
    flags: u32,
    tex_addr: usize,
    tex_w: u16,
    tex_h: u16,
    blend: BlendMode,
) {
    let depth_test = flags & TRI_DEPTH_TEST != 0;
    let depth_write = flags & TRI_DEPTH_WRITE != 0;
    let gouraud = flags & TRI_GOURAUD != 0;
    let textured = flags & TRI_TEXTURED != 0 && tex_w > 0 && tex_h > 0;

    let min_x = v[0].x.min(v[1].x).min(v[2].x).max(clip.0);
    let max_x = v[0].x.max(v[1].x).max(v[2].x).min(clip.2);
    let min_y = v[0].y.min(v[1].y).min(v[2].y).max(clip.1);
    let max_y = v[0].y.max(v[1].y).max(v[2].y).min(clip.3);
    if min_x > max_x || min_y > max_y {
        return;
    }
    let area = edge(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
    if area == 0 {
        return;
    }
    let area_f = area as f64;

    for y in min_y..=max_y {
        for x in min_x..=max_x {
            let w0 = edge(v[1].x, v[1].y, v[2].x, v[2].y, x, y);
            let w1 = edge(v[2].x, v[2].y, v[0].x, v[0].y, x, y);
            let w2 = edge(v[0].x, v[0].y, v[1].x, v[1].y, x, y);
            let inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if !inside {
                continue;
            }
            let b0 = w0 as f64 / area_f;
            let b1 = w1 as f64 / area_f;
            let b2 = w2 as f64 / area_f;

            let z = (b0 * v[0].z as f64 + b1 * v[1].z as f64 + b2 * v[2].z as f64) as u32;

            let zoff = zbuf.map(|za| za + (y as usize * fb_w + x as usize) * 2);
            if let Some(zoff) = zoff {
                if zoff + 2 > data.len() {
                    continue;
                }
                let cur = u16::from_le_bytes([data[zoff], data[zoff + 1]]);
                if depth_test && (z as u16) >= cur {
                    continue;
                }
            }

            let mut color = if gouraud {
                lerp3_color(v[0].color, v[1].color, v[2].color, b0, b1, b2)
            } else {
                v[0].color
            };

            if textured {
                let u = (b0 * v[0].u as f64 + b1 * v[1].u as f64 + b2 * v[2].u as f64).clamp(0.0, 65535.0) as u32;
                let vv = (b0 * v[0].v as f64 + b1 * v[1].v as f64 + b2 * v[2].v as f64).clamp(0.0, 65535.0) as u32;
                let tx = ((u * tex_w as u32) >> 16).min(tex_w as u32 - 1) as usize;
                let ty = ((vv * tex_h as u32) >> 16).min(tex_h as u32 - 1) as usize;
                let toff = tex_addr + (ty * tex_w as usize + tx) * 4;
                if toff + 4 <= data.len() {
                    let texel = read_u32(data, toff);
                    color = if gouraud { modulate(color, texel) } else { texel };
                }
            }

            let off = fb + (y as usize) * stride + (x as usize) * 4;
            if off + 4 > data.len() {
                continue;
            }
            let out = if blend == BlendMode::Opaque {
                color
            } else {
                blend_pixel(read_u32(data, off), color, blend)
            };
            write_pixel(data, off, out);

            if depth_write {
                if let Some(zoff) = zoff {
                    data[zoff..zoff + 2].copy_from_slice(&(z as u16).to_le_bytes());
                }
            }
        }
    }
}

impl GpuMmio {
    pub fn new(base: u32, vram: SharedVram) -> Self {
        let mut regs = Registers::default();
        regs.blend_mode = 0;
        Self { base, regs, vram }
    }

    /// Llamado por el host (loop de presentación) cada vez que se muestra
    /// un frame; deja un flag pulsado que el software puede sondear vía
    /// STATUS (se limpia al leerlo), útil para esperar vsync sin tearing.
    pub fn pulse_vblank(&mut self) {
        self.regs.vblank = true;
    }

    pub fn display_fb_addr(&self) -> u32 {
        if self.regs.display_fb == 1 && self.regs.fb1_addr != 0 {
            self.regs.fb1_addr
        } else {
            self.regs.fb0_addr
        }
    }

    pub fn fb_width(&self) -> u16 {
        self.regs.fb_width
    }
    pub fn fb_height(&self) -> u16 {
        self.regs.fb_height
    }

    #[inline]
    fn fb_stride(&self) -> usize {
        self.regs.fb_width as usize * 4
    }

    #[inline]
    fn draw_fb_addr(&self) -> usize {
        (if self.regs.draw_fb == 1 && self.regs.fb1_addr != 0 {
            self.regs.fb1_addr
        } else {
            self.regs.fb0_addr
        }) as usize
    }

    #[inline]
    fn clip_rect(&self) -> Clip {
        let w = self.regs.fb_width as i32;
        let h = self.regs.fb_height as i32;
        if self.regs.clip_x1 == 0 && self.regs.clip_y1 == 0 {
            (0, 0, w - 1, h - 1)
        } else {
            (
                self.regs.clip_x0.max(0),
                self.regs.clip_y0.max(0),
                self.regs.clip_x1.min(w - 1),
                self.regs.clip_y1.min(h - 1),
            )
        }
    }

    #[inline]
    fn blend(&self) -> BlendMode {
        BlendMode::from_u32(self.regs.blend_mode)
    }

    fn palette_color(&self, data: &[u8], idx: u8) -> u32 {
        let idx = (idx & 0x0F) as usize;
        if self.regs.text_palette_addr == 0 {
            DEFAULT_PALETTE[idx]
        } else {
            let off = self.regs.text_palette_addr as usize + idx * 4;
            if off + 4 <= data.len() {
                read_u32(data, off)
            } else {
                DEFAULT_PALETTE[idx]
            }
        }
    }

    // ─────────────────────── primitivas individuales ──────────────────

    fn op_clear(&mut self, data: &mut [u8], color: u32) {
        let fb = self.draw_fb_addr();
        let stride = self.fb_stride();
        let w = self.regs.fb_width as usize;
        let h = self.regs.fb_height as usize;
        for y in 0..h {
            let mut p = fb + y * stride;
            if p + w * 4 > data.len() {
                break;
            }
            for _ in 0..w {
                write_pixel(data, p, color);
                p += 4;
            }
        }
    }

    fn op_clear_z(&mut self, data: &mut [u8], value: u32) {
        if self.regs.zbuf_addr == 0 {
            return;
        }
        let za = self.regs.zbuf_addr as usize;
        let w = self.regs.fb_width as usize;
        let h = self.regs.fb_height as usize;
        let zv = (value as u16).to_le_bytes();
        let mut p = za;
        for _ in 0..(w * h) {
            if p + 2 > data.len() {
                break;
            }
            data[p..p + 2].copy_from_slice(&zv);
            p += 2;
        }
    }

    fn op_grad_x(&mut self, data: &mut [u8], left: u32, right: u32) {
        let fb = self.draw_fb_addr();
        let stride = self.fb_stride();
        let w = self.regs.fb_width as usize;
        let h = self.regs.fb_height as usize;
        let denom = w.saturating_sub(1).max(1);
        for y in 0..h {
            let mut p = fb + y * stride;
            for x in 0..w {
                if p + 4 > data.len() {
                    break;
                }
                write_pixel(data, p, lerp_channel(left, right, x, denom));
                p += 4;
            }
        }
    }

    fn op_grad_y(&mut self, data: &mut [u8], top: u32, bottom: u32) {
        let fb = self.draw_fb_addr();
        let stride = self.fb_stride();
        let w = self.regs.fb_width as usize;
        let h = self.regs.fb_height as usize;
        let denom = h.saturating_sub(1).max(1);
        for y in 0..h {
            let c = lerp_channel(top, bottom, y, denom);
            let mut p = fb + y * stride;
            for _ in 0..w {
                if p + 4 > data.len() {
                    break;
                }
                write_pixel(data, p, c);
                p += 4;
            }
        }
    }

    fn op_grad_xy(&mut self, data: &mut [u8], c00: u32, c10: u32, c01: u32, c11: u32) {
        let fb = self.draw_fb_addr();
        let stride = self.fb_stride();
        let w = self.regs.fb_width as usize;
        let h = self.regs.fb_height as usize;
        let dx = w.saturating_sub(1).max(1);
        let dy = h.saturating_sub(1).max(1);
        for y in 0..h {
            let left = lerp_channel(c00, c01, y, dy);
            let right = lerp_channel(c10, c11, y, dy);
            let mut p = fb + y * stride;
            for x in 0..w {
                if p + 4 > data.len() {
                    break;
                }
                write_pixel(data, p, lerp_channel(left, right, x, dx));
                p += 4;
            }
        }
    }

    fn op_putchar(&mut self, data: &mut [u8], x: i32, y: i32, ch: u16, fg: u32, bg: u32) {
        let fw = self.regs.font_w as i32;
        let fh = self.regs.font_h as i32;
        if fw <= 0 || fh <= 0 {
            return;
        }
        let glyph_bytes_per_row = ((fw + 7) / 8) as usize;
        let glyph_size = glyph_bytes_per_row * fh as usize;
        let glyph_off = self.regs.font_addr as usize + (ch as usize) * glyph_size;
        let fb = self.draw_fb_addr();
        let stride = self.fb_stride();
        let clip = self.clip_rect();
        let blend = self.blend();

        for j in 0..fh {
            let row_off = glyph_off + j as usize * glyph_bytes_per_row;
            if row_off >= data.len() {
                break;
            }
            let row_byte = data[row_off];
            for i in 0..fw {
                let bit = (row_byte >> (7 - (i & 7))) & 1;
                // Celda opaca (como una terminal real): siempre pinta fg u
                // bg, sin "transparencia mágica" por valor especial. Si
                // hace falta compositar sin tapar el fondo, es un blit con
                // BLIT_ALPHA, no un caso especial acá.
                let color = if bit != 0 { fg } else { bg };
                put_pixel(data, fb, stride, clip, x + i, y + j, color, blend);
            }
        }
    }

    fn op_puts(&mut self, data: &mut [u8], x: i32, y: i32, fg: u32, bg: u32, str_addr: u32, len: u32) {
        let fw = self.regs.font_w as i32;
        let base = str_addr as usize;
        for k in 0..len as usize {
            if base + k >= data.len() {
                break;
            }
            let ch = data[base + k] as u16;
            self.op_putchar(data, x + k as i32 * fw, y, ch, fg, bg);
        }
    }

    fn op_blit(&mut self, data: &mut [u8], src_addr: u32, src_w: u16, src_h: u16, dst_x: i32, dst_y: i32, flags: u32, key: u32) {
        let fb = self.draw_fb_addr();
        let stride = self.fb_stride();
        let clip = self.clip_rect();
        let use_key = flags & BLIT_KEY != 0;
        let use_alpha = flags & BLIT_ALPHA != 0;
        let flip_x = flags & BLIT_FLIP_X != 0;
        let flip_y = flags & BLIT_FLIP_Y != 0;
        let blend = if use_alpha { BlendMode::Alpha } else { BlendMode::Opaque };

        let sw = src_w as i32;
        let sh = src_h as i32;
        let src_base = src_addr as usize;

        for j in 0..sh {
            let sy = if flip_y { sh - 1 - j } else { j };
            for i in 0..sw {
                let sx = if flip_x { sw - 1 - i } else { i };
                let soff = src_base + (sy as usize * sw as usize + sx as usize) * 4;
                if soff + 4 > data.len() {
                    continue;
                }
                let texel = read_u32(data, soff);
                if use_key && texel == key {
                    continue;
                }
                put_pixel(data, fb, stride, clip, dst_x + i, dst_y + j, texel, blend);
            }
        }
    }

    fn op_triangle3d(&mut self, data: &mut [u8], words: &[u32]) {
        let flags = words[0];
        let tex_addr = words[1] as usize;
        let tex_w = (words[2] & 0xFFFF) as u16;
        let tex_h = (words[2] >> 16) as u16;
        let mut verts = [Vertex3 { x: 0, y: 0, z: 0, color: 0, u: 0, v: 0 }; 3];
        for n in 0..3 {
            let o = 3 + n * 6;
            verts[n] = Vertex3 {
                x: words[o] as i32,
                y: words[o + 1] as i32,
                z: words[o + 2],
                color: words[o + 3],
                u: words[o + 4] as i32,
                v: words[o + 5] as i32,
            };
        }
        let fb = self.draw_fb_addr();
        let stride = self.fb_stride();
        let clip = self.clip_rect();
        let zbuf = if self.regs.zbuf_addr != 0 { Some(self.regs.zbuf_addr as usize) } else { None };
        let blend = if flags & TRI_BLEND != 0 { self.blend() } else { BlendMode::Opaque };
        fill_triangle3d_raw(
            data, fb, stride, clip, zbuf, self.regs.fb_width as usize, verts, flags, tex_addr, tex_w, tex_h, blend,
        );
    }

    /// Compone el layer de texto (si está habilitado) sobre el back buffer
    /// y, si hay double buffering, intercambia front/back. Se ejecuta con
    /// el comando FLIP.
    fn op_flip(&mut self, data: &mut [u8]) {
        if self.regs.text_enable && self.regs.text_addr != 0 {
            let fw = self.regs.font_w as i32;
            let fh = self.regs.font_h as i32;
            if fw > 0 && fh > 0 {
                let cols = self.regs.text_cols as usize;
                let rows = self.regs.text_rows as usize;
                let cell_base = self.regs.text_addr as usize;
                for row in 0..rows {
                    for col in 0..cols {
                        let off = cell_base + (row * cols + col) * 2;
                        if off + 2 > data.len() {
                            continue;
                        }
                        let ch = data[off] as u16;
                        if ch == 0 {
                            continue; // celda "vacía": no toca el framebuffer (convención de texto)
                        }
                        let attr = data[off + 1];
                        let fg = self.palette_color(data, attr & 0x0F);
                        let bg = self.palette_color(data, attr >> 4);
                        self.op_putchar(data, col as i32 * fw, row as i32 * fh, ch, fg, bg);
                    }
                }
            }
        }

        if self.regs.fb1_addr != 0 {
            self.regs.display_fb = self.regs.draw_fb;
            self.regs.draw_fb = 1 - self.regs.draw_fb;
        }
    }

    fn exec_command_buffer(&mut self) {
        let vram = self.vram.clone();
        let mut guard = vram.borrow_mut();
        let data = &mut guard.data;

        let start = self.regs.cmd_addr as usize;
        let len_words = self.regs.cmd_len as usize;
        let Some(end) = start.checked_add(len_words.saturating_mul(4)) else { return };
        if end > data.len() {
            return;
        }

        let mut cursor = start;
        while cursor + 4 <= end {
            let op = read_u32(data, cursor);
            let args = cursor + 4;
            macro_rules! need {
                ($n:expr) => {{
                    let bytes = ($n) * 4;
                    if args + bytes > end {
                        break;
                    }
                    bytes
                }};
            }
            match op {
                OP_NOP => {
                    cursor = args;
                }
                OP_CLEAR => {
                    need!(1);
                    let color = read_u32(data, args);
                    self.op_clear(data, color);
                    cursor = args + 4;
                }
                OP_CLEAR_Z => {
                    need!(1);
                    let v = read_u32(data, args);
                    self.op_clear_z(data, v);
                    cursor = args + 4;
                }
                OP_SET_CLIP => {
                    need!(4);
                    self.regs.clip_x0 = read_u32(data, args) as i32;
                    self.regs.clip_y0 = read_u32(data, args + 4) as i32;
                    self.regs.clip_x1 = read_u32(data, args + 8) as i32;
                    self.regs.clip_y1 = read_u32(data, args + 12) as i32;
                    cursor = args + 16;
                }
                OP_SET_BLEND => {
                    need!(1);
                    self.regs.blend_mode = read_u32(data, args);
                    cursor = args + 4;
                }
                OP_FLIP => {
                    self.op_flip(data);
                    cursor = args;
                }
                OP_FILLRECT | OP_RECT_OUTLINE => {
                    need!(5);
                    let x = read_u32(data, args) as i32;
                    let y = read_u32(data, args + 4) as i32;
                    let w = read_u32(data, args + 8) as i32;
                    let h = read_u32(data, args + 12) as i32;
                    let color = read_u32(data, args + 16);
                    let fb = self.draw_fb_addr();
                    let stride = self.fb_stride();
                    let clip = self.clip_rect();
                    let blend = self.blend();
                    if op == OP_FILLRECT {
                        fill_rect_raw(data, fb, stride, clip, x, y, w, h, color, blend);
                    } else {
                        let x1 = x + w - 1;
                        let y1 = y + h - 1;
                        line_raw(data, fb, stride, clip, x, y, x1, y, color, blend);
                        line_raw(data, fb, stride, clip, x, y1, x1, y1, color, blend);
                        line_raw(data, fb, stride, clip, x, y, x, y1, color, blend);
                        line_raw(data, fb, stride, clip, x1, y, x1, y1, color, blend);
                    }
                    cursor = args + 20;
                }
                OP_LINE => {
                    need!(5);
                    let x0 = read_u32(data, args) as i32;
                    let y0 = read_u32(data, args + 4) as i32;
                    let x1 = read_u32(data, args + 8) as i32;
                    let y1 = read_u32(data, args + 12) as i32;
                    let color = read_u32(data, args + 16);
                    let fb = self.draw_fb_addr();
                    let stride = self.fb_stride();
                    let clip = self.clip_rect();
                    let blend = self.blend();
                    line_raw(data, fb, stride, clip, x0, y0, x1, y1, color, blend);
                    cursor = args + 20;
                }
                OP_CIRCLE => {
                    need!(5);
                    let cx = read_u32(data, args) as i32;
                    let cy = read_u32(data, args + 4) as i32;
                    let r = read_u32(data, args + 8) as i32;
                    let color = read_u32(data, args + 12);
                    let filled = read_u32(data, args + 16) != 0;
                    let fb = self.draw_fb_addr();
                    let stride = self.fb_stride();
                    let clip = self.clip_rect();
                    let blend = self.blend();
                    circle_raw(data, fb, stride, clip, cx, cy, r, color, filled, blend);
                    cursor = args + 20;
                }
                OP_TRIANGLE2D => {
                    need!(8);
                    let pts = [
                        (read_u32(data, args) as i32, read_u32(data, args + 4) as i32),
                        (read_u32(data, args + 8) as i32, read_u32(data, args + 12) as i32),
                        (read_u32(data, args + 16) as i32, read_u32(data, args + 20) as i32),
                    ];
                    let color = read_u32(data, args + 24);
                    let filled = read_u32(data, args + 28) != 0;
                    let fb = self.draw_fb_addr();
                    let stride = self.fb_stride();
                    let clip = self.clip_rect();
                    let blend = self.blend();
                    if filled {
                        fill_triangle2d_raw(data, fb, stride, clip, pts, color, blend);
                    } else {
                        for i in 0..3 {
                            let (x0, y0) = pts[i];
                            let (x1, y1) = pts[(i + 1) % 3];
                            line_raw(data, fb, stride, clip, x0, y0, x1, y1, color, blend);
                        }
                    }
                    cursor = args + 32;
                }
                OP_GRAD_X => {
                    need!(2);
                    let l = read_u32(data, args);
                    let r = read_u32(data, args + 4);
                    self.op_grad_x(data, l, r);
                    cursor = args + 8;
                }
                OP_GRAD_Y => {
                    need!(2);
                    let t = read_u32(data, args);
                    let b = read_u32(data, args + 4);
                    self.op_grad_y(data, t, b);
                    cursor = args + 8;
                }
                OP_GRAD_XY => {
                    need!(4);
                    let c00 = read_u32(data, args);
                    let c10 = read_u32(data, args + 4);
                    let c01 = read_u32(data, args + 8);
                    let c11 = read_u32(data, args + 12);
                    self.op_grad_xy(data, c00, c10, c01, c11);
                    cursor = args + 16;
                }
                OP_PUTCHAR => {
                    need!(5);
                    let x = read_u32(data, args) as i32;
                    let y = read_u32(data, args + 4) as i32;
                    let ch = read_u32(data, args + 8) as u16;
                    let fg = read_u32(data, args + 12);
                    let bg = read_u32(data, args + 16);
                    self.op_putchar(data, x, y, ch, fg, bg);
                    cursor = args + 20;
                }
                OP_PUTS => {
                    need!(6);
                    let x = read_u32(data, args) as i32;
                    let y = read_u32(data, args + 4) as i32;
                    let fg = read_u32(data, args + 8);
                    let bg = read_u32(data, args + 12);
                    let str_addr = read_u32(data, args + 16);
                    let len = read_u32(data, args + 20);
                    self.op_puts(data, x, y, fg, bg, str_addr, len);
                    cursor = args + 24;
                }
                OP_BLIT => {
                    need!(7);
                    let src_addr = read_u32(data, args);
                    let src_w = read_u32(data, args + 4) as u16;
                    let src_h = read_u32(data, args + 8) as u16;
                    let dst_x = read_u32(data, args + 12) as i32;
                    let dst_y = read_u32(data, args + 16) as i32;
                    let flags = read_u32(data, args + 20);
                    let key = read_u32(data, args + 24);
                    self.op_blit(data, src_addr, src_w, src_h, dst_x, dst_y, flags, key);
                    cursor = args + 28;
                }
                OP_TRIANGLE3D => {
                    need!(21);
                    let mut words = [0u32; 21];
                    for (n, w) in words.iter_mut().enumerate() {
                        *w = read_u32(data, args + n * 4);
                    }
                    self.op_triangle3d(data, &words);
                    cursor = args + 21 * 4;
                }
                _ => break, // opcode desconocido: cortamos limpio en vez de desincronizar el parser
            }
        }
    }

    #[inline]
    fn within(&self, paddr: u32) -> Option<u32> {
        let off = paddr.wrapping_sub(self.base);
        if off < 0x100 { Some(off) } else { None }
    }
}

impl Device for GpuMmio {
    fn range(&self) -> RangeInclusive<u32> {
        self.base..=self.base + 0xFF
    }

    fn read8(&mut self, paddr: u32) -> MemResult<u8> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };
        Ok(match off {
            0x00 => (self.regs.fb_width & 0xFF) as u8,
            0x01 => (self.regs.fb_width >> 8) as u8,
            0x02 => (self.regs.fb_height & 0xFF) as u8,
            0x03 => (self.regs.fb_height >> 8) as u8,
            0x08..=0x0B => ((self.regs.fb0_addr >> ((off - 0x08) * 8)) & 0xFF) as u8,
            0x0C..=0x0F => ((self.regs.fb1_addr >> ((off - 0x0C) * 8)) & 0xFF) as u8,
            0x10..=0x13 => ((self.regs.zbuf_addr >> ((off - 0x10) * 8)) & 0xFF) as u8,
            0x14..=0x17 => {
                let status = (self.regs.vblank as u32) << 1
                    | (self.regs.draw_fb as u32) << 2
                    | (self.regs.display_fb as u32) << 3;
                if off == 0x14 {
                    self.regs.vblank = false; // clear-on-read
                }
                ((status >> ((off - 0x14) * 8)) & 0xFF) as u8
            }
            0x18..=0x1B => ((self.regs.cmd_addr >> ((off - 0x18) * 8)) & 0xFF) as u8,
            0x1C..=0x1F => ((self.regs.cmd_len >> ((off - 0x1C) * 8)) & 0xFF) as u8,
            0x24..=0x27 => ((self.regs.font_addr >> ((off - 0x24) * 8)) & 0xFF) as u8,
            0x28 => self.regs.font_w,
            0x29 => self.regs.font_h,
            0x30..=0x33 => ((self.regs.text_addr >> ((off - 0x30) * 8)) & 0xFF) as u8,
            0x34 => (self.regs.text_cols & 0xFF) as u8,
            0x35 => (self.regs.text_cols >> 8) as u8,
            0x36 => (self.regs.text_rows & 0xFF) as u8,
            0x37 => (self.regs.text_rows >> 8) as u8,
            0x38..=0x3B => ((self.regs.text_palette_addr >> ((off - 0x38) * 8)) & 0xFF) as u8,
            0x3C => self.regs.text_enable as u8,
            _ => 0,
        })
    }

    fn write8(&mut self, paddr: u32, value: u8) -> MemResult<()> {
        let Some(off) = self.within(paddr) else {
            return Err(MemoryError::Unmapped(paddr));
        };

        macro_rules! rmw32 {
            ($field:expr, $base:expr) => {{
                let shift = (off - $base) * 8;
                let mask = !(0xFFu32 << shift);
                $field = ($field & mask) | ((value as u32) << shift);
            }};
        }

        match off {
            0x00 => self.regs.fb_width = (self.regs.fb_width & 0xFF00) | value as u16,
            0x01 => self.regs.fb_width = (self.regs.fb_width & 0x00FF) | ((value as u16) << 8),
            0x02 => self.regs.fb_height = (self.regs.fb_height & 0xFF00) | value as u16,
            0x03 => self.regs.fb_height = (self.regs.fb_height & 0x00FF) | ((value as u16) << 8),
            0x08..=0x0B => rmw32!(self.regs.fb0_addr, 0x08),
            0x0C..=0x0F => rmw32!(self.regs.fb1_addr, 0x0C),
            0x10..=0x13 => rmw32!(self.regs.zbuf_addr, 0x10),
            0x18..=0x1B => rmw32!(self.regs.cmd_addr, 0x18),
            0x1C..=0x1F => rmw32!(self.regs.cmd_len, 0x1C),
            0x20 => self.exec_command_buffer(), // CMD_KICK: cualquier escritura dispara la lista
            0x24..=0x27 => rmw32!(self.regs.font_addr, 0x24),
            0x28 => self.regs.font_w = value,
            0x29 => self.regs.font_h = value,
            0x30..=0x33 => rmw32!(self.regs.text_addr, 0x30),
            0x34 => self.regs.text_cols = (self.regs.text_cols & 0xFF00) | value as u16,
            0x35 => self.regs.text_cols = (self.regs.text_cols & 0x00FF) | ((value as u16) << 8),
            0x36 => self.regs.text_rows = (self.regs.text_rows & 0xFF00) | value as u16,
            0x37 => self.regs.text_rows = (self.regs.text_rows & 0x00FF) | ((value as u16) << 8),
            0x38..=0x3B => rmw32!(self.regs.text_palette_addr, 0x38),
            0x3C => self.regs.text_enable = value != 0,
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
    use crate::devices::vram::new_shared_vram;

    const BASE: u32 = 0x1F80_2000;
    const CMDBUF: u32 = 0x10000;

    fn setup(w: u16, h: u16) -> (GpuMmio, SharedVram) {
        let vram = new_shared_vram(1024 * 1024);
        let mut gpu = GpuMmio::new(BASE, vram.clone());
        gpu.write16(BASE + 0x00, w).unwrap();
        gpu.write16(BASE + 0x02, h).unwrap();
        gpu.write32(BASE + 0x08, 0).unwrap(); // FB0_ADDR = 0
        gpu.write8(BASE + 0x28, 8).unwrap(); // FONT_W
        gpu.write8(BASE + 0x29, 8).unwrap(); // FONT_H
        (gpu, vram)
    }

    fn push_words(vram: &SharedVram, addr: u32, words: &[u32]) {
        let mut v = vram.borrow_mut();
        for (i, w) in words.iter().enumerate() {
            let off = addr as usize + i * 4;
            v.data[off..off + 4].copy_from_slice(&w.to_le_bytes());
        }
    }

    fn kick(gpu: &mut GpuMmio, vram: &SharedVram, words: &[u32]) {
        push_words(vram, CMDBUF, words);
        gpu.write32(BASE + 0x18, CMDBUF).unwrap(); // CMD_ADDR
        gpu.write32(BASE + 0x1C, words.len() as u32).unwrap(); // CMD_LEN
        gpu.write8(BASE + 0x20, 1).unwrap(); // CMD_KICK
    }

    fn pixel(vram: &SharedVram, fb_off: u32, w: u16, x: u32, y: u32) -> u32 {
        let v = vram.borrow();
        let off = fb_off as usize + (y as usize * w as usize + x as usize) * 4;
        read_u32(&v.data, off)
    }

    #[test]
    fn clear_fills_whole_framebuffer() {
        let (mut gpu, vram) = setup(4, 4);
        kick(&mut gpu, &vram, &[OP_CLEAR, 0xFF112233]);
        assert_eq!(pixel(&vram, 0, 4, 0, 0), 0xFF112233);
        assert_eq!(pixel(&vram, 0, 4, 3, 3), 0xFF112233);
    }

    #[test]
    fn fillrect_respects_clip() {
        let (mut gpu, vram) = setup(8, 8);
        kick(&mut gpu, &vram, &[OP_SET_CLIP, 0, 0, 3, 3]);
        kick(&mut gpu, &vram, &[OP_FILLRECT, 0, 0, 8, 8, 0xFFAABBCC]);
        assert_eq!(pixel(&vram, 0, 8, 0, 0), 0xFFAABBCC);
        assert_eq!(pixel(&vram, 0, 8, 3, 3), 0xFFAABBCC);
        assert_eq!(pixel(&vram, 0, 8, 4, 4), 0); // fuera del clip, intacto
    }

    #[test]
    fn flip_swaps_front_and_back_when_double_buffered() {
        let (mut gpu, vram) = setup(4, 4);
        gpu.write32(BASE + 0x0C, 0x1000).unwrap(); // FB1_ADDR != 0 -> habilita double buffer
        let status_before = gpu.read8(BASE + 0x14).unwrap();
        assert_eq!((status_before >> 2) & 1, 0); // draw_fb arranca en 0

        kick(&mut gpu, &vram, &[OP_FLIP]);

        let status_after = gpu.read8(BASE + 0x14).unwrap();
        assert_eq!((status_after >> 2) & 1, 1); // draw pasó al otro buffer
        assert_eq!((status_after >> 3) & 1, 0); // display sigue mostrando el que se acababa de llenar
    }

    #[test]
    fn blit_with_color_key_skips_matching_pixels() {
        let (mut gpu, vram) = setup(4, 4);
        // sprite 2x1 en offset 0x2000: un pixel rojo, un pixel "key" (magenta)
        let key = 0xFFFF00FFu32;
        push_words(&vram, 0x2000, &[0xFFFF0000, key]);
        kick(&mut gpu, &vram, &[OP_CLEAR, 0xFF000000]);
        kick(
            &mut gpu,
            &vram,
            &[OP_BLIT, 0x2000, 2, 1, 0, 0, BLIT_KEY, key],
        );
        assert_eq!(pixel(&vram, 0, 4, 0, 0), 0xFFFF0000); // se dibujó
        assert_eq!(pixel(&vram, 0, 4, 1, 0), 0xFF000000); // key: se preservó el fondo
    }

    #[test]
    fn triangle3d_depth_test_rejects_farther_pixel() {
        let (mut gpu, vram) = setup(8, 8);
        gpu.write32(BASE + 0x10, 0x3000).unwrap(); // ZBUF_ADDR
        // Z-buffer arranca en 0 (todo "más cerca" que cualquier u16),
        // así que limpiarlo a 0xFFFF (lejos) antes de dibujar.
        kick(&mut gpu, &vram, &[OP_CLEAR_Z, 0xFFFF]);

        let flags = TRI_DEPTH_TEST | TRI_DEPTH_WRITE;
        // triángulo grande cerca (z chico) cubriendo (0,0)-(0,7)-(7,0)
        kick(
            &mut gpu,
            &vram,
            &[
                OP_TRIANGLE3D, flags, 0, 0,
                0, 0, 10, 0xFF00FF00, 0, 0,
                0, 7, 10, 0xFF00FF00, 0, 0,
                7, 0, 10, 0xFF00FF00, 0, 0,
            ],
        );
        assert_eq!(pixel(&vram, 0, 8, 1, 1), 0xFF00FF00);

        // mismo triángulo pero más lejos (z mayor): no debería pisar lo ya dibujado
        kick(
            &mut gpu,
            &vram,
            &[
                OP_TRIANGLE3D, flags, 0, 0,
                0, 0, 200, 0xFF0000FF, 0, 0,
                0, 7, 200, 0xFF0000FF, 0, 0,
                7, 0, 200, 0xFF0000FF, 0, 0,
            ],
        );
        assert_eq!(pixel(&vram, 0, 8, 1, 1), 0xFF00FF00); // se mantiene el más cercano
    }

    #[test]
    fn text_mode_composes_on_flip() {
        let (mut gpu, vram) = setup(16, 8);
        gpu.write32(BASE + 0x30, 0x4000).unwrap(); // TEXT_ADDR
        gpu.write16(BASE + 0x34, 2).unwrap(); // TEXT_COLS
        gpu.write16(BASE + 0x36, 1).unwrap(); // TEXT_ROWS
        gpu.write8(BASE + 0x3C, 1).unwrap(); // TEXT_ENABLE

        // celda (0,0): char='A' (0x41), attr = bg=0(negro) fg=15(blanco)
        {
            let mut v = vram.borrow_mut();
            v.data[0x4000] = 0x41;
            v.data[0x4001] = 0x0F;
        }

        kick(&mut gpu, &vram, &[OP_CLEAR, 0xFF000000]);
        kick(&mut gpu, &vram, &[OP_FLIP]);

        // No verificamos el glifo exacto (depende de la fuente, que acá
        // está vacía/cero), pero el compose no debe paniquear y debe poder
        // leer la celda sin reventar límites.
        assert_eq!(pixel(&vram, 0, 16, 10, 5), 0xFF000000);
    }
}
