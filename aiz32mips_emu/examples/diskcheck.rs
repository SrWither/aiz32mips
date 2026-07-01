use aiz32mips_core::devices::storage::StorageMmio;
use aiz32mips_core::memory::Device;
use std::env;

fn main() {
    let path = env::args().nth(1).unwrap_or_else(|| "../kernel/disk.img".to_string());
    let block: u32 = env::args().nth(2).map(|s| s.parse().unwrap()).unwrap_or(8098);
    let mut dev = StorageMmio::new(0x1F80_3000, std::path::Path::new(&path)).unwrap();
    dev.write32(0x1F80_3000, block).unwrap();
    dev.write32(0x1F80_3004, 1).unwrap(); // CMD_READ
    let status = dev.read32(0x1F80_3008).unwrap();
    println!("status={status}");
    let mut buf = [0u8; 32];
    for (i, b) in buf.iter_mut().enumerate() {
        *b = dev.read8(0x1F80_3200 + i as u32).unwrap();
    }
    println!("{:02X?}", buf);
}
