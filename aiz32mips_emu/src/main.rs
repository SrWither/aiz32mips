mod mmio_offsets;
mod ui;

use std::env;
use std::fs;
use std::process;

use aiz32mips_core::cpu::CPU;
use aiz32mips_core::devices::gpu::{self, GpuMmio};
use aiz32mips_core::devices::vram::{new_shared_vram, GpuVram};
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

    // === GPU + VRAM ===
    // VRAM compartida entre el device de bus (para que el CPU le pueda
    // escribir directamente con loads/stores normales, p.ej. para efectos)
    // y la GPU (que la usa para ejecutar comandos). Antes esto se resolvía
    // con un puntero crudo a un struct que además se movía de lugar (UB);
    // ahora ambos lados comparten la misma asignación vía Rc<RefCell<>>.
    let vram_base = 0x1000_0000;
    let vram_size = 4 * 1024 * 1024; // 4MB
    let vram_buf = new_shared_vram(vram_size);
    let vram_dev = GpuVram::new(vram_base, vram_buf.clone());
    let gpu = GpuMmio::new(GPU_MMIO_PHYS, vram_buf.clone()); // registrado en su dirección física;
    // el CPU la ve reflejada en kseg1 vía GPU_MMIO_BASE (ver mmio_offsets.rs)

    // font ROM, en el layout default que también usa la lib en C
    let font_addr = gpu::VRAM_FONT as usize;
    vram_buf.borrow_mut().data[font_addr..font_addr + font_rom_data.len()]
        .copy_from_slice(&font_rom_data);

    // registrar en bus
    bus.add_device(Box::new(vram_dev));
    bus.add_device(Box::new(gpu));

    // Config inicial: 320x200, framebuffer único (sin double buffer todavía)
    write16(&mut bus, REG_FB_WIDTH, 320);
    write16(&mut bus, REG_FB_HEIGHT, 200);
    write32(&mut bus, REG_FB0_ADDR, gpu::VRAM_FB0);
    write32(&mut bus, REG_FONTADDR, gpu::VRAM_FONT);
    write8(&mut bus, REG_FONTW, 8);
    write8(&mut bus, REG_FONTH, 8);

    // === cpu ===
    let mut cpu = CPU::new();

    // === sdl ===
    // present_from_bus ya no recorre la VRAM byte a byte por el bus (256k
    // accesos individuales por frame); copia directo del buffer compartido.
    let mut sdl = SdlDisplay::new(3, vram_buf)?; // escala x3

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

    // El polling de eventos (pump_events_quit) y el present van juntos,
    // una vez cada `present_every` instrucciones — antes se llamaba
    // pump_events_quit() en CADA instrucción de CPU, creando un EventPump
    // nuevo y haciendo un poll de SDL/X11 millones de veces por segundo.
    // Eso, no el intérprete, era el cuello de botella real del "1 fps"
    // (el intérprete solo ya anda a varios millones de instrucciones/seg).
    if infinite {
        loop {
            cpu.step(&mut bus);
            steps_since_present += 1;
            if steps_since_present >= present_every {
                steps_since_present = 0;
                if sdl.pump_events_quit() {
                    break;
                }
                present_and_pulse_vblank(&mut sdl, &mut bus);
            }
        }
    } else {
        for _ in 0..cycles {
            cpu.step(&mut bus);
            steps_since_present += 1;
            if steps_since_present >= present_every {
                steps_since_present = 0;
                if sdl.pump_events_quit() {
                    break;
                }
                present_and_pulse_vblank(&mut sdl, &mut bus);
            }
        }
        // presenta al final
        present_and_pulse_vblank(&mut sdl, &mut bus);
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

// Presenta el frame actual y le pulsa VBLANK a la GPU (STATUS bit1), para
// que el software pueda hacer `gpu_wait_vblank()` sin tearing.
fn present_and_pulse_vblank(sdl: &mut SdlDisplay, bus: &mut MemoryBus) {
    let _ = sdl.present_from_bus(bus);
    if let Some(gpu) = bus.device_mut::<GpuMmio>() {
        gpu.pulse_vblank();
    }
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
