mod mmio_offsets;
mod ui;

use std::env;
use std::fs;
use std::process;

use aiz32mips_core::cpu::CPU;
use aiz32mips_core::devices::gpu::GpuMmio;
use aiz32mips_core::devices::vram::GpuVram;
use aiz32mips_core::devices::{ram::Ram, rom::Rom};
use aiz32mips_core::memory::MemoryBus;

use mmio_offsets::*;
use ui::display::SdlDisplay;

fn main() -> anyhow::Result<()> {
    // === args ===
    let args: Vec<String> = env::args().collect();
    if args.len() < 4 {
        eprintln!(
            "Uso: {} <rom_path.bin> <font_rom.bin> <ciclos|inf>",
            args[0]
        );
        process::exit(1);
    }
    let rom_path = &args[1];
    let font_rom_path = &args[2];
    let cycles_arg = &args[3];

    // === rom ===
    let rom_data = fs::read(rom_path)
        .map_err(|e| anyhow::anyhow!("Error al leer ROM '{}': {}", rom_path, e))?;

    let font_rom_data = fs::read(font_rom_path)
        .map_err(|e| anyhow::anyhow!("Error al leer ROM de fuentes '{}': {}", font_rom_path, e))?;

    // === bus ===
    let mut bus = MemoryBus::new(true); // little-endian
    bus.add_device(Box::new(Ram::new(0x0000_0000, 0x0020_0000))); // 2MB
    bus.add_device(Box::new(Rom::new(0x1FC0_0000, rom_data))); // BIOS

    // === GPU ===
    let vram_base = 0x1000_0000;
    let vram_size = 4 * 1024 * 1024; // 4MB
    let fb_off = 0; // FB al inicio
    let mut vram = GpuVram::new(vram_base, vram_size);
    let gpu = GpuMmio::new(GPU_MMIO_BASE, &mut vram);

    // font ROM
    let font_addr = 0x0020_0000;
    vram.slice_mut()[font_addr..font_addr + font_rom_data.len()].copy_from_slice(&font_rom_data);

    // registrar en bus
    bus.add_device(Box::new(vram));
    bus.add_device(Box::new(gpu));

    // Config inicial simple: 320x200x32
    write16(&mut bus, REG_WIDTH, 320);
    write16(&mut bus, REG_HEIGHT, 200);
    write16(&mut bus, REG_PITCH, 320);
    write8(&mut bus, REG_BPP, 32);
    write32(&mut bus, REG_FBADDR, fb_off);
    write32(&mut bus, REG_FONTADDR, font_addr as u32);

    // === cpu ===
    let mut cpu = CPU::new();

    // === sdl ===
    let mut sdl = SdlDisplay::new(3)?; // escala x3

    // === ciclos ===
    let infinite = cycles_arg == "inf";
    let cycles: u64 = if infinite {
        0
    } else {
        cycles_arg.parse().unwrap_or(10_000)
    };
    println!(
        "[AIZ32] Ejecutando ROM '{}' por {} ciclos...",
        rom_path,
        if infinite {
            "∞".to_string()
        } else {
            cycles.to_string()
        }
    );

    // === loop ===
    let mut steps_since_present = 0u32;
    let present_every = 10_000; // ajustá esto según rendimiento

    if infinite {
        loop {
            if sdl.pump_events_quit() {
                break;
            }
            cpu.step(&mut bus);
            steps_since_present += 1;
            if steps_since_present >= present_every {
                steps_since_present = 0;
                let _ = sdl.present_from_bus(&mut bus); // ignoramos error vram no configurado
            }
        }
    } else {
        for _ in 0..cycles {
            if sdl.pump_events_quit() {
                break;
            }
            cpu.step(&mut bus);
            steps_since_present += 1;
            if steps_since_present >= present_every {
                steps_since_present = 0;
                let _ = sdl.present_from_bus(&mut bus);
            }
        }
        // presenta al final
        let _ = sdl.present_from_bus(&mut bus);
    }

    // dump
    println!("\n--- CPU Registers Dump (R0–R9 en decimal) ---");
    for i in 0..10 {
        let val = cpu.registers.read(i);
        println!("R{:02} = {}", i, val as i32);
    }
    println!("HI = {}", cpu.registers.special.hi as i32);
    println!("LO = {}", cpu.registers.special.lo as i32);
    println!("PC = 0x{:08X}", cpu.registers.get_pc());
    println!("SP = 0x{:08X}", cpu.registers.get_sp());

    Ok(())
}

// helpers MMIO
fn write8(bus: &mut MemoryBus, addr: u32, v: u8) {
    let _ = bus.write8_virt(addr, v);
}
fn write16(bus: &mut MemoryBus, addr: u32, v: u16) {
    let _ = bus.write16_virt(addr, v);
}
fn write32(bus: &mut MemoryBus, addr: u32, v: u32) {
    let _ = bus.write32_virt(addr, v);
}
