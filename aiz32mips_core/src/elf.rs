//! Parser mínimo de ELF32 (little-endian, MIPS) para cargar binarios sin
//! pasar por `objcopy -O binary`. Sólo entiende lo que hace falta para
//! levantar los PT_LOAD de un ejecutable: no hay relocación, ni symtab, ni
//! soporte para ELF64/big-endian/otras arquitecturas.

const EI_CLASS_32: u8 = 1;
const EI_DATA_LSB: u8 = 1;
const EM_MIPS: u16 = 8;
const PT_LOAD: u32 = 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ElfError {
    NotElf,
    Not32Bit,
    NotLittleEndian,
    NotMips,
    Truncated,
}

/// Un segmento cargable, ya con el BSS (memsz - filesz) rellenado en cero.
#[derive(Debug, Clone)]
pub struct ElfSegment {
    pub vaddr: u32,
    /// Dirección física resuelta a partir de vaddr (resta el base de
    /// kseg0/kseg1 si corresponde; en kuseg queda igual, asumiendo mapeo
    /// identidad como hace un loader que corre antes de que exista TLB).
    pub paddr: u32,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct ElfImage {
    pub entry: u32,
    pub segments: Vec<ElfSegment>,
}

#[inline]
pub fn is_elf(data: &[u8]) -> bool {
    data.len() >= 4 && data[0..4] == [0x7F, b'E', b'L', b'F']
}

/// Resuelve una dirección virtual a la física "de arranque" (sin TLB):
/// kseg0/kseg1 restan su base, kuseg y el resto quedan igual.
pub fn default_paddr(vaddr: u32) -> u32 {
    match vaddr {
        0x8000_0000..=0x9FFF_FFFF => vaddr - 0x8000_0000, // kseg0
        0xA000_0000..=0xBFFF_FFFF => vaddr - 0xA000_0000, // kseg1
        _ => vaddr,
    }
}

pub fn parse_elf32(data: &[u8]) -> Result<ElfImage, ElfError> {
    if !is_elf(data) {
        return Err(ElfError::NotElf);
    }
    if data.len() < 52 {
        return Err(ElfError::Truncated);
    }
    if data[4] != EI_CLASS_32 {
        return Err(ElfError::Not32Bit);
    }
    if data[5] != EI_DATA_LSB {
        return Err(ElfError::NotLittleEndian);
    }

    let rd_u16 = |o: usize| -> Result<u16, ElfError> {
        data.get(o..o + 2)
            .map(|b| u16::from_le_bytes([b[0], b[1]]))
            .ok_or(ElfError::Truncated)
    };
    let rd_u32 = |o: usize| -> Result<u32, ElfError> {
        data.get(o..o + 4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
            .ok_or(ElfError::Truncated)
    };

    let e_machine = rd_u16(18)?;
    if e_machine != EM_MIPS {
        return Err(ElfError::NotMips);
    }
    let e_entry = rd_u32(24)?;
    let e_phoff = rd_u32(28)? as usize;
    let e_phentsize = rd_u16(42)? as usize;
    let e_phnum = rd_u16(44)? as usize;

    let mut segments = Vec::new();
    for i in 0..e_phnum {
        let ph = e_phoff + i * e_phentsize;
        let p_type = rd_u32(ph)?;
        if p_type != PT_LOAD {
            continue;
        }
        let p_offset = rd_u32(ph + 4)? as usize;
        let p_vaddr = rd_u32(ph + 8)?;
        let p_filesz = rd_u32(ph + 16)? as usize;
        let p_memsz = rd_u32(ph + 20)? as usize;

        let file_bytes = data
            .get(p_offset..p_offset + p_filesz)
            .ok_or(ElfError::Truncated)?;
        let mut buf = vec![0u8; p_memsz.max(p_filesz)];
        buf[..p_filesz].copy_from_slice(file_bytes);

        segments.push(ElfSegment {
            vaddr: p_vaddr,
            paddr: default_paddr(p_vaddr),
            data: buf,
        });
    }

    Ok(ElfImage {
        entry: e_entry,
        segments,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Arma un ELF32 LE MIPS mínimo con un solo PT_LOAD (código + un poco
    /// de BSS al final), para no depender de un binario de fixture externo.
    fn build_test_elf(vaddr: u32, entry: u32, code: &[u8], bss_extra: u32) -> Vec<u8> {
        const EHSIZE: usize = 52;
        const PHENTSIZE: usize = 32;
        let phoff = EHSIZE as u32;
        let data_off = EHSIZE + PHENTSIZE;

        let mut buf = vec![0u8; data_off + code.len()];
        // e_ident
        buf[0..4].copy_from_slice(&[0x7F, b'E', b'L', b'F']);
        buf[4] = EI_CLASS_32;
        buf[5] = EI_DATA_LSB;
        buf[6] = 1; // EV_CURRENT
        // e_type (ET_EXEC=2), e_machine (EM_MIPS=8)
        buf[16..18].copy_from_slice(&2u16.to_le_bytes());
        buf[18..20].copy_from_slice(&EM_MIPS.to_le_bytes());
        buf[20..24].copy_from_slice(&1u32.to_le_bytes()); // e_version
        buf[24..28].copy_from_slice(&entry.to_le_bytes()); // e_entry
        buf[28..32].copy_from_slice(&phoff.to_le_bytes()); // e_phoff
        buf[42..44].copy_from_slice(&(PHENTSIZE as u16).to_le_bytes()); // e_phentsize
        buf[44..46].copy_from_slice(&1u16.to_le_bytes()); // e_phnum

        // program header (Elf32_Phdr) en phoff
        let ph = phoff as usize;
        buf[ph..ph + 4].copy_from_slice(&PT_LOAD.to_le_bytes());
        buf[ph + 4..ph + 8].copy_from_slice(&(data_off as u32).to_le_bytes()); // p_offset
        buf[ph + 8..ph + 12].copy_from_slice(&vaddr.to_le_bytes()); // p_vaddr
        buf[ph + 12..ph + 16].copy_from_slice(&vaddr.to_le_bytes()); // p_paddr
        buf[ph + 16..ph + 20].copy_from_slice(&(code.len() as u32).to_le_bytes()); // p_filesz
        buf[ph + 20..ph + 24]
            .copy_from_slice(&((code.len() as u32) + bss_extra).to_le_bytes()); // p_memsz

        buf[data_off..data_off + code.len()].copy_from_slice(code);
        buf
    }

    #[test]
    fn parses_entry_and_single_segment() {
        let code = [0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02];
        let elf = build_test_elf(0x8000_1000, 0x8000_1004, &code, 4);

        assert!(is_elf(&elf));
        let img = parse_elf32(&elf).unwrap();

        assert_eq!(img.entry, 0x8000_1004);
        assert_eq!(img.segments.len(), 1);

        let seg = &img.segments[0];
        assert_eq!(seg.vaddr, 0x8000_1000);
        assert_eq!(seg.paddr, 0x0000_1000); // kseg0: resta 0x8000_0000
        assert_eq!(&seg.data[0..6], &code);
        assert_eq!(seg.data.len(), code.len() + 4); // + BSS en cero
        assert_eq!(&seg.data[6..10], &[0, 0, 0, 0]);
    }

    #[test]
    fn kseg1_segment_resolves_to_low_physical() {
        let elf = build_test_elf(0xBFC0_0000, 0xBFC0_0000, &[0x01, 0x02, 0x03, 0x04], 0);
        let img = parse_elf32(&elf).unwrap();
        assert_eq!(img.segments[0].paddr, 0x1FC0_0000);
    }

    #[test]
    fn rejects_non_elf() {
        assert_eq!(parse_elf32(&[0, 1, 2, 3]).unwrap_err(), ElfError::NotElf);
    }

    #[test]
    fn rejects_wrong_machine() {
        let mut elf = build_test_elf(0x8000_0000, 0x8000_0000, &[0xAA], 0);
        elf[18..20].copy_from_slice(&3u16.to_le_bytes()); // EM_386, no MIPS
        assert_eq!(parse_elf32(&elf).unwrap_err(), ElfError::NotMips);
    }
}
