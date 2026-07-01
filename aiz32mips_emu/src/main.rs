mod mmio_offsets;
mod ui;

use std::env;
use std::fs;
use std::process;
use std::time::{Duration, Instant};

use aiz32mips_core::cop::CAUSE_IP2;
use aiz32mips_core::cpu::CPU;
use aiz32mips_core::devices::gpu::{self, GpuMmio};
use aiz32mips_core::devices::keyboard::KeyboardMmio;
use aiz32mips_core::devices::storage::StorageMmio;
use aiz32mips_core::devices::vram::{new_shared_vram, GpuVram};
use aiz32mips_core::devices::ram::Ram;
use aiz32mips_core::elf;
use aiz32mips_core::memory::MemoryBus;

use mmio_offsets::*;
use ui::display::SdlDisplay;

const RAM_BASE: u32 = 0x0000_0000;
const RAM_SIZE: usize = 0x0800_0000; // 128MB
const ROM_BASE: u32 = 0x1FC0_0000;
const ROM_SIZE: usize = 0x0100_0000; // 16MB

fn main() -> anyhow::Result<()> {
    // === args ===
    let args: Vec<String> = env::args().collect();
    if args.len() < 4 {
        eprintln!(
            "Uso: {} <rom_path.bin|.elf> <font_rom.bin> <ciclos|inf> [disk_img]",
            args[0]
        );
        process::exit(1);
    }
    let rom_path = &args[1];
    let font_rom_path = &args[2];
    let cycles_arg = &args[3];
    let disk_img_path = args.get(4).map(String::as_str).unwrap_or("disk.img");

    // === imagen inicial de RAM/ROM ===
    // Soporta tanto un .bin plano (comportamiento de siempre: se copia
    // entero a la ROM en 0xBFC00000, arranca ahí) como un .elf real (cada
    // PT_LOAD se copia a donde diga su dirección física resuelta — RAM o
    // ROM según en qué segmento caiga — y arranca en e_entry).
    let file_data = fs::read(rom_path)
        .map_err(|e| anyhow::anyhow!("Error al leer ROM '{}': {}", rom_path, e))?;

    let font_rom_data = fs::read(font_rom_path)
        .map_err(|e| anyhow::anyhow!("Error al leer ROM de fuentes '{}': {}", font_rom_path, e))?;

    let mut ram_image = vec![0u8; RAM_SIZE];
    let mut rom_image = vec![0u8; ROM_SIZE];
    let mut entry_pc: Option<u32> = None;

    if elf::is_elf(&file_data) {
        let img = elf::parse_elf32(&file_data)
            .map_err(|e| anyhow::anyhow!("ELF '{}' inválido: {:?}", rom_path, e))?;
        println!(
            "[AIZ32] '{}' es un ELF32/MIPS: entry={:#010X}, {} segmento(s) PT_LOAD",
            rom_path,
            img.entry,
            img.segments.len()
        );
        for seg in &img.segments {
            blit_segment(&mut ram_image, &mut rom_image, seg.paddr, &seg.data).map_err(|e| {
                anyhow::anyhow!(
                    "segmento ELF vaddr={:#010X} paddr={:#010X} (+{} bytes): {}",
                    seg.vaddr,
                    seg.paddr,
                    seg.data.len(),
                    e
                )
            })?;
        }
        entry_pc = Some(img.entry);
    } else {
        if file_data.len() > ROM_SIZE {
            return Err(anyhow::anyhow!(
                "'{}' ({} bytes) no entra en la ROM ({} bytes)",
                rom_path,
                file_data.len(),
                ROM_SIZE
            ));
        }
        rom_image[..file_data.len()].copy_from_slice(&file_data);
    }

    // === bus ===
    let mut bus = MemoryBus::new(true); // little-endian
    bus.add_device(Box::new(Ram::from_data(RAM_BASE, ram_image)));
    // "ROM" de arranque, pero respaldada por Ram (escribible): el toolchain
    // no separa .data/.bss del resto con un linker script real (no hay
    // crt0 que copie nada a RAM al arrancar), así que el linker mete las
    // variables globales/estáticas ahí mismo, contiguas al código. Con un
    // device de sólo lectura esas escrituras se perdían en silencio (según
    // la convención "escribir a ROM no hace nada"), rompiendo cualquier
    // estado global mutable — p.ej. el cursor de una consola o el estado
    // de teclas sostenidas de un sprite.
    bus.add_device(Box::new(Ram::from_data(ROM_BASE, rom_image))); // BIOS

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

    // === teclado ===
    bus.add_device(Box::new(KeyboardMmio::new(KBD_MMIO_PHYS)));

    // === storage ===
    // Respaldado por un archivo real: lo que el kernel escriba sobrevive a
    // que se cierre el emulador (ver aiz32mips_core::devices::storage).
    let storage = StorageMmio::new(STORAGE_MMIO_PHYS, std::path::Path::new(disk_img_path))
        .map_err(|e| anyhow::anyhow!("no pude abrir el disco '{}': {}", disk_img_path, e))?;
    bus.add_device(Box::new(storage));

    // Config inicial: 320x200, framebuffer único (sin double buffer todavía)
    write16(&mut bus, REG_FB_WIDTH, 320);
    write16(&mut bus, REG_FB_HEIGHT, 200);
    write32(&mut bus, REG_FB0_ADDR, gpu::VRAM_FB0);
    write32(&mut bus, REG_FONTADDR, gpu::VRAM_FONT);
    write8(&mut bus, REG_FONTW, 8);
    write8(&mut bus, REG_FONTH, 8);

    // === cpu ===
    let mut cpu = CPU::new();
    if let Some(entry) = entry_pc {
        cpu.registers.set_pc(entry);
        cpu.registers.write(31, entry); // $ra: igual de "inofensivo" que en reset() si loopea para siempre
    }

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
    // Sondear input tiene que pasar seguido (si no, la ventana queda sin
    // procesar sus eventos de sistema el tiempo suficiente como para que
    // el SO la marque "no responde"). El present/vblank, en cambio, se
    // gatea por RELOJ DE PARED (¬1/60s), no por cantidad de instrucciones:
    // un conteo fijo de instrucciones paga a velocidades muy distintas
    // según la máquina/build (debug vs release), así que un juego que hace
    // gpu_wait_vblank() por frame (ver user/cube3d.c) terminaba girando a
    // la velocidad que le tocara en instrucciones, no a 60fps de verdad.
    let mut steps_since_poll = 0u32;
    let poll_every: u32 = 20_000;
    let frame_interval = Duration::from_secs_f64(1.0 / 60.0);
    let mut last_present = Instant::now();

    if infinite {
        loop {
            cpu.step(&mut bus);
            steps_since_poll += 1;
            if steps_since_poll >= poll_every {
                steps_since_poll = 0;
                if poll_input(&mut sdl, &mut bus, &mut cpu) {
                    break;
                }
                if last_present.elapsed() >= frame_interval {
                    present_and_pulse_vblank(&mut sdl, &mut bus);
                    last_present = Instant::now();
                }
            }
        }
    } else {
        for _ in 0..cycles {
            cpu.step(&mut bus);
            steps_since_poll += 1;
            if steps_since_poll >= poll_every {
                steps_since_poll = 0;
                if poll_input(&mut sdl, &mut bus, &mut cpu) {
                    break;
                }
                if last_present.elapsed() >= frame_interval {
                    present_and_pulse_vblank(&mut sdl, &mut bus);
                    last_present = Instant::now();
                }
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

// Copia los bytes de un segmento PT_LOAD a donde corresponda (RAM o ROM
// según en qué rango físico caiga). Fuera de esos dos rangos no hay dónde
// ponerlo: para este emulador bare-metal, esos son los únicos dos bancos.
fn blit_segment(ram: &mut [u8], rom: &mut [u8], paddr: u32, data: &[u8]) -> anyhow::Result<()> {
    let end = paddr as u64 + data.len() as u64;
    if paddr >= RAM_BASE && end <= RAM_BASE as u64 + ram.len() as u64 {
        let off = (paddr - RAM_BASE) as usize;
        ram[off..off + data.len()].copy_from_slice(data);
        Ok(())
    } else if paddr >= ROM_BASE && end <= ROM_BASE as u64 + rom.len() as u64 {
        let off = (paddr - ROM_BASE) as usize;
        rom[off..off + data.len()].copy_from_slice(data);
        Ok(())
    } else {
        Err(anyhow::anyhow!("no entra ni en RAM ni en ROM"))
    }
}

// Junta los eventos de SDL: le pasa las teclas al KeyboardMmio y refleja su
// IRQ en Cause.IP2 (nivel, no flanco). Devuelve true si hay que salir.
fn poll_input(sdl: &mut SdlDisplay, bus: &mut MemoryBus, cpu: &mut CPU) -> bool {
    let pump = sdl.pump_events();
    if let Some(kbd) = bus.device_mut::<KeyboardMmio>() {
        for ev in pump.key_events {
            kbd.push_event(ev);
        }
        if kbd.has_pending_irq() {
            cpu.cop0.set_cause_bit(CAUSE_IP2);
        } else {
            cpu.cop0.clear_cause_bit(CAUSE_IP2);
        }
    }
    pump.quit
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
