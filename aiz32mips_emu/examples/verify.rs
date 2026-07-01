// Herramienta de verificación temporal (sin SDL): corre un .bin contra el
// CPU/GPU real y vuelca algunos píxeles del framebuffer para confirmar que
// los comandos realmente se ejecutaron. `cargo run --example verify -- <bin>`
use std::env;
use std::fs;

use aiz32mips_core::cpu::CPU;
use aiz32mips_core::devices::gpu::{self, GpuMmio};
use aiz32mips_core::devices::vram::{new_shared_vram, GpuVram};
use aiz32mips_core::devices::{ram::Ram, rom::Rom};
use aiz32mips_core::memory::MemoryBus;

fn main() {
    let args: Vec<String> = env::args().collect();
    let bin_path = &args[1];
    let cycles: u64 = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(500_000);

    let rom_data = fs::read(bin_path).unwrap();
    let font_data = fs::read("font_gen/font_rom.bin").unwrap_or_default();

    let mut bus = MemoryBus::new(true);
    bus.add_device(Box::new(Ram::new(0x0000_0000, 0x0020_0000)));
    bus.add_device(Box::new(Rom::new(0x1FC0_0000, rom_data)));

    let vram_buf = new_shared_vram(4 * 1024 * 1024);
    let vram_dev = GpuVram::new(0x1000_0000, vram_buf.clone());
    let gpu = GpuMmio::new(0x1F80_2000, vram_buf.clone());
    if !font_data.is_empty() {
        let off = gpu::VRAM_FONT as usize;
        vram_buf.borrow_mut().data[off..off + font_data.len()].copy_from_slice(&font_data);
    }
    bus.add_device(Box::new(vram_dev));
    bus.add_device(Box::new(gpu));

    let mut cpu = CPU::new();
    let trace = env::var("TRACE").is_ok();
    for i in 0..cycles {
        if trace && i < 200 {
            println!("[{i}] PC={:#010X}", cpu.registers.get_pc());
        }
        cpu.step(&mut bus);
    }

    let v = vram_buf.borrow();
    let px = |x: usize, y: usize| -> u32 {
        let off = (y * 320 + x) * 4;
        u32::from_le_bytes(v.data[off..off + 4].try_into().unwrap())
    };
    println!("PC final = {:#010X}", cpu.registers.get_pc());
    println!("pixel(0,0)     = {:#010X}", px(0, 0));
    println!("pixel(159,100) = {:#010X}", px(159, 100));
    println!("pixel(319,199) = {:#010X}", px(319, 199));
    println!(
        "primeros 8 bytes de VRAM_CMDBUF: {:02X?}",
        &v.data[gpu::VRAM_CMDBUF as usize..gpu::VRAM_CMDBUF as usize + 8]
    );

    let bg = px(0, 0);
    let mut distinct_non_bg = 0u32;
    for y in 0..200usize {
        for x in 0..320usize {
            if px(x, y) != bg {
                distinct_non_bg += 1;
            }
        }
    }
    println!("pixeles distintos del de (0,0): {distinct_non_bg}");
}
