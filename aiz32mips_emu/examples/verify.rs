// Herramienta de verificación temporal (sin SDL): corre un .bin contra el
// CPU/GPU real y vuelca algunos píxeles del framebuffer para confirmar que
// los comandos realmente se ejecutaron. `cargo run --example verify -- <bin>`
use std::env;
use std::fs;

use aiz32mips_core::cop::CAUSE_IP2;
use aiz32mips_core::cpu::CPU;
use aiz32mips_core::devices::gpu::{self, GpuMmio};
use aiz32mips_core::devices::keyboard::{self, KeyboardMmio};
use aiz32mips_core::devices::storage::StorageMmio;
use aiz32mips_core::devices::vram::{new_shared_vram, GpuVram};
use aiz32mips_core::devices::ram::Ram;
use aiz32mips_core::elf;
use aiz32mips_core::memory::MemoryBus;

const RAM_BASE: u32 = 0x0000_0000;
const RAM_SIZE: usize = 0x0800_0000; // 128MB
const ROM_BASE: u32 = 0x1FC0_0000;
const ROM_SIZE: usize = 0x0100_0000; // 16MB

fn blit_segment(ram: &mut [u8], rom: &mut [u8], paddr: u32, data: &[u8]) {
    let end = paddr as u64 + data.len() as u64;
    if paddr >= RAM_BASE && end <= RAM_BASE as u64 + ram.len() as u64 {
        let off = (paddr - RAM_BASE) as usize;
        ram[off..off + data.len()].copy_from_slice(data);
    } else if paddr >= ROM_BASE && end <= ROM_BASE as u64 + rom.len() as u64 {
        let off = (paddr - ROM_BASE) as usize;
        rom[off..off + data.len()].copy_from_slice(data);
    } else {
        panic!("segmento en {paddr:#010X} no entra en RAM ni ROM");
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let bin_path = &args[1];
    let cycles: u64 = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(500_000);

    let file_data = fs::read(bin_path).unwrap();
    let font_data = fs::read("font_gen/font_rom.bin")
        .or_else(|_| fs::read("../font_gen/font_rom.bin"))
        .unwrap_or_default();
    if font_data.is_empty() {
        eprintln!("[verify] AVISO: no encontré font_rom.bin, los glyphs van a salir en blanco");
    }

    let mut ram_image = vec![0u8; RAM_SIZE];
    let mut rom_image = vec![0u8; ROM_SIZE];
    let mut entry_pc = None;

    if elf::is_elf(&file_data) {
        let img = elf::parse_elf32(&file_data).expect("ELF inválido");
        for seg in &img.segments {
            blit_segment(&mut ram_image, &mut rom_image, seg.paddr, &seg.data);
        }
        entry_pc = Some(img.entry);
        println!("cargado como ELF, entry={:#010X}", img.entry);
    } else {
        rom_image[..file_data.len()].copy_from_slice(&file_data);
    }

    let mut bus = MemoryBus::new(true);
    bus.add_device(Box::new(Ram::from_data(RAM_BASE, ram_image)));
    bus.add_device(Box::new(Ram::from_data(ROM_BASE, rom_image)));

    let vram_buf = new_shared_vram(4 * 1024 * 1024);
    let vram_dev = GpuVram::new(0x1000_0000, vram_buf.clone());
    let gpu = GpuMmio::new(0x1F80_2000, vram_buf.clone());
    if !font_data.is_empty() {
        let off = gpu::VRAM_FONT as usize;
        vram_buf.borrow_mut().data[off..off + font_data.len()].copy_from_slice(&font_data);
    }
    bus.add_device(Box::new(vram_dev));
    bus.add_device(Box::new(gpu));
    bus.add_device(Box::new(KeyboardMmio::new(0x1F80_1000)));

    // Storage respaldado por un archivo real (default: verify_disk.img en
    // el cwd), configurable con DISK_IMG — permite probar persistencia
    // real corriendo este binario dos veces con la misma ruta.
    let disk_path = env::var("DISK_IMG").unwrap_or_else(|_| "verify_disk.img".to_string());
    let storage = StorageMmio::new(0x1F80_3000, std::path::Path::new(&disk_path))
        .expect("no pude abrir/crear el disco de DISK_IMG");
    bus.add_device(Box::new(storage));

    // Inyección de teclado sintética para probar sin SDL:
    //   KBD_TEXT=abc      -> eventos de texto (kind=1), uno por char
    //   KBD_HOLD=RIGHT    -> un evento de tecla cruda "pressed" (sin release)
    if let Ok(text) = env::var("KBD_TEXT") {
        if let Some(kbd) = bus.device_mut::<KeyboardMmio>() {
            for b in text.bytes() {
                kbd.push_event(keyboard::EVT_KIND_TEXT | b as u32);
            }
        }
    }
    if let Ok(key) = env::var("KBD_HOLD") {
        let kc = match key.as_str() {
            "UP" => keyboard::KC_UP,
            "DOWN" => keyboard::KC_DOWN,
            "LEFT" => keyboard::KC_LEFT,
            "RIGHT" => keyboard::KC_RIGHT,
            _ => 0,
        };
        if let Some(kbd) = bus.device_mut::<KeyboardMmio>() {
            kbd.push_event(kc as u32 | keyboard::EVT_PRESSED);
        }
    }

    let mut cpu = CPU::new();
    if let Some(entry) = entry_pc {
        cpu.registers.set_pc(entry);
        cpu.registers.write(31, entry);
    }
    let trace = env::var("TRACE").is_ok();
    for i in 0..cycles {
        if trace && i < 200 {
            println!("[{i}] PC={:#010X}", cpu.registers.get_pc());
        }
        // Igual que hace main.rs en el emulador real: reflejar el IRQ
        // pendiente del device en Cause.IP2 antes de cada step.
        if let Some(kbd) = bus.device_mut::<KeyboardMmio>() {
            if kbd.has_pending_irq() {
                cpu.cop0.set_cause_bit(CAUSE_IP2);
            } else {
                cpu.cop0.clear_cause_bit(CAUSE_IP2);
            }
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

    let bg = 0xFF000000u32;
    let mut distinct_non_bg = 0u32;
    for y in 0..200usize {
        for x in 0..320usize {
            if px(x, y) != bg {
                distinct_non_bg += 1;
            }
        }
    }

    if let Ok(n) = env::var("DUMP_TEXT_ROWS") {
        let n: usize = n.parse().unwrap_or(3);
        let cols = 40usize;
        let base = gpu::VRAM_TEXT as usize;
        for row in 0..n {
            let mut s = String::new();
            for col in 0..cols {
                let off = base + (row * cols + col) * 2;
                let ch = v.data[off];
                s.push(if ch == 0 { '.' } else { ch as char });
            }
            println!("row{row}: {s}");
        }
    }
    println!("pixeles distintos del de (0,0): {distinct_non_bg}");
}
