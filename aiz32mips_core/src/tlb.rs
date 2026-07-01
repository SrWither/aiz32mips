/// TLB de 16 entradas (tamaño típico de cores MIPS32 chicos, p.ej. 4Kc).
pub const TLB_ENTRIES: usize = 16;

/// Solo se soportan páginas de 4KB: PageMask se guarda para que TLBR lo
/// pueda leer de vuelta tal cual se escribió, pero el lookup ignora su
/// contenido y siempre compara el VPN2 completo (19 bits).
#[derive(Debug, Clone, Copy, Default)]
pub struct TlbEntry {
    pub vpn2: u32, // vaddr[31:13]
    pub asid: u8,
    pub global: bool,
    pub page_mask: u32,
    pub pfn0: u32,
    pub d0: bool,
    pub v0: bool,
    pub pfn1: u32,
    pub d1: bool,
    pub v1: bool,
}

#[derive(Clone, Copy)]
pub struct Tlb {
    pub entries: [TlbEntry; TLB_ENTRIES],
}

impl Default for Tlb {
    fn default() -> Self {
        Self {
            entries: [TlbEntry::default(); TLB_ENTRIES],
        }
    }
}

impl Tlb {
    pub fn new() -> Self {
        Self::default()
    }

    /// Busca una entrada cuyo VPN2 coincida con `vaddr` y cuyo ASID
    /// coincida (o que sea Global). Devuelve el índice de la primera
    /// coincidencia, igual que hardware real.
    pub fn find(&self, vaddr: u32, asid: u8) -> Option<usize> {
        let vpn2 = vaddr >> 13;
        self.entries
            .iter()
            .position(|e| e.vpn2 == vpn2 && (e.global || e.asid == asid))
    }
}
