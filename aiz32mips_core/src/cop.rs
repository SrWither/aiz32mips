pub const STATUS_IE: u32 = 1 << 0;
pub const STATUS_EXL: u32 = 1 << 1;
pub const STATUS_ERL: u32 = 1 << 2;
pub const STATUS_KSU_MASK: u32 = 0b11 << 3; // bits 3-4
pub const STATUS_KSU_USER: u32 = 0b10 << 3;
pub const STATUS_CU0: u32 = 1 << 28; // CP0 usable en modo usuario
pub const STATUS_BEV: u32 = 1 << 22;

pub const CAUSE_EXC_MASK: u32 = 0x7C; // bits 2-6
pub const CAUSE_CE_MASK: u32 = 0x3 << 28; // coprocesador que disparó CpU
pub const CAUSE_BD: u32 = 1 << 31;

// CP0 register numbers (numeración estándar MIPS32)
pub const CP0_REG_INDEX: usize = 0;
pub const CP0_REG_RANDOM: usize = 1;
pub const CP0_REG_ENTRYLO0: usize = 2;
pub const CP0_REG_ENTRYLO1: usize = 3;
pub const CP0_REG_CONTEXT: usize = 4;
pub const CP0_REG_PAGEMASK: usize = 5;
pub const CP0_REG_WIRED: usize = 6;
pub const CP0_REG_BADVADDR: usize = 8;
pub const CP0_REG_ENTRYHI: usize = 10;

// Campo IP (interrupts pending) de Cause / IM (interrupt mask) de Status:
// bits 8-15, alineados 1 a 1 (IP0..IP7 con IM0..IM7).
pub const CAUSE_IP_MASK: u32 = 0xFF00;
pub const CAUSE_IP_SW_MASK: u32 = 0x0300; // IP0/IP1: únicos bits de Cause escribibles por software (MTC0)
pub const CAUSE_IP7: u32 = 1 << 15; // interrupción de timer (Count == Compare)

// CP0 register numbers usados por el pipeline de excepciones/interrupciones
pub const CP0_REG_COUNT: usize = 9;
pub const CP0_REG_COMPARE: usize = 11;
pub const CP0_REG_CAUSE: usize = 13;

pub struct Cop0 {
    pub regs: [u32; 32],
}

pub struct Cop1 {
    regs: [u64; 32], // bits crudos del FPR
    pub fcsr: u32,
    pub fir: u32,
}


impl Default for Cop0 {
    fn default() -> Self {
        Cop0 { regs: [0; 32] }
    }
}

impl Default for Cop1 {
    fn default() -> Self {
        Cop1 {
            regs: [0u64; 32],
            fcsr: 0,
            fir: 0,
        }
    }
}

impl Cop0 {
    pub fn new() -> Self {
        Self::default()
    }

    #[inline]
    pub fn read(&self, index: usize) -> u32 {
        self.regs[index]
    }

    #[inline]
    pub fn write(&mut self, index: usize, value: u32) {
        self.regs[index] = value;
    }

    #[inline]
    pub fn status(&self) -> u32 {
        self.regs[12]
    }

    #[inline]
    pub fn set_status(&mut self, value: u32) {
        self.regs[12] = value;
    }

    #[inline]
    pub fn cause(&self) -> u32 {
        self.regs[13]
    }

    #[inline]
    pub fn epc(&self) -> u32 {
        self.regs[14]
    }

    #[inline]
    pub fn set_epc(&mut self, value: u32) {
        self.regs[14] = value;
    }

    #[inline]
    pub fn badvaddr(&self) -> u32 {
        self.regs[8]
    }

    #[inline]
    pub fn set_badvaddr(&mut self, value: u32) {
        self.regs[8] = value;
    }

    #[inline]
    pub fn set_cause_exc_code(&mut self, code: u32) {
        self.regs[13] = (self.regs[13] & !CAUSE_EXC_MASK) | ((code << 2) & CAUSE_EXC_MASK);
    }

    #[inline]
    pub fn set_cause_bd(&mut self, bd: bool) {
        if bd {
            self.regs[13] |= CAUSE_BD;
        } else {
            self.regs[13] &= !CAUSE_BD;
        }
    }

    #[inline]
    pub fn set_cause_bit(&mut self, bit: u32) {
        self.regs[13] |= bit;
    }

    #[inline]
    pub fn clear_cause_bit(&mut self, bit: u32) {
        self.regs[13] &= !bit;
    }

    /// IE=1, EXL=0 y ERL=0: única combinación en la que el core puede tomar
    /// una interrupción.
    #[inline]
    pub fn interrupts_enabled(&self) -> bool {
        self.status_bit(STATUS_IE) && !self.status_bit(STATUS_EXL) && !self.status_bit(STATUS_ERL)
    }

    /// Intersección de Cause.IP con Status.IM: interrupciones pendientes Y habilitadas.
    #[inline]
    pub fn pending_enabled_interrupts(&self) -> u32 {
        self.regs[13] & self.regs[12] & CAUSE_IP_MASK
    }

    /// Avanza el contador libre Count en una unidad y, si alcanza a Compare,
    /// deja pendiente la interrupción de timer (IP7). Real: corre siempre,
    /// incluso con EXL/ERL activos.
    pub fn tick_timer(&mut self) {
        let count = self.regs[CP0_REG_COUNT].wrapping_add(1);
        self.regs[CP0_REG_COUNT] = count;
        if count == self.regs[CP0_REG_COMPARE] {
            self.set_cause_bit(CAUSE_IP7);
        }
    }

    pub fn dump(&self) {
        println!("--- COP0 Registers ---");
        for (i, r) in self.regs.iter().enumerate() {
            println!("COP0[{:02}] = 0x{:08X}", i, r);
        }
    }

    pub fn set_status_bit(&mut self, bit: u32) {
        self.regs[12] |= bit;
    }

    pub fn clear_status_bit(&mut self, bit: u32) {
        self.regs[12] &= !bit;
    }

    pub fn status_bit(&self, bit: u32) -> bool {
        (self.regs[12] & bit) != 0
    }

    /// Modo usuario real: KSU=10 y ni EXL ni ERL activos (ambos fuerzan
    /// modo kernel sin importar KSU, como en hardware real).
    #[inline]
    pub fn is_user_mode(&self) -> bool {
        !self.status_bit(STATUS_EXL)
            && !self.status_bit(STATUS_ERL)
            && (self.regs[12] & STATUS_KSU_MASK) == STATUS_KSU_USER
    }

    #[inline]
    pub fn set_cause_ce(&mut self, coprocessor: u8) {
        self.regs[13] =
            (self.regs[13] & !CAUSE_CE_MASK) | (((coprocessor as u32) << 28) & CAUSE_CE_MASK);
    }
}

impl Cop1 {
    pub fn new() -> Self { Self::default() }

    // ===== Acceso formato S (single, 32-bit) =====
    #[inline]
    pub fn read_s(&self, i: usize) -> f32 {
        f32::from_bits(self.regs[i] as u32)
    }
    #[inline]
    pub fn write_s(&mut self, i: usize, v: f32) {
        self.regs[i] = v.to_bits() as u64; // high = 0 (o 0xFFFF_FFFF para NaN-boxing)
    }
    #[inline]
    pub fn read_s_bits(&self, i: usize) -> u32 {
        self.regs[i] as u32
    }
    #[inline]
    pub fn write_s_bits(&mut self, i: usize, bits: u32) {
        self.regs[i] = bits as u64; // high = 0
    }

    // ===== Acceso formato D (double, 64-bit) =====
    #[inline]
    pub fn read_d(&self, i: usize) -> f64 {
        f64::from_bits(self.regs[i])
    }
    #[inline]
    pub fn write_d(&mut self, i: usize, v: f64) {
        self.regs[i] = v.to_bits();
    }
    #[inline]
    pub fn read_d_bits(&self, i: usize) -> u64 {
        self.regs[i]
    }
    #[inline]
    pub fn write_d_bits(&mut self, i: usize, bits: u64) {
        self.regs[i] = bits;
    }

    // ===== Compat (si tenías llamadas genéricas) =====
    // Úsalas SOLO para formato D (mantengo nombres para no romper mucho)
    #[inline]
    pub fn read_f(&self, index: usize) -> f64 {
        self.read_d(index)
    }
    #[inline]
    pub fn write_f(&mut self, index: usize, value: f64) {
        self.write_d(index, value)
    }
    #[inline]
    pub fn read_bits(&self, index: usize) -> u64 {
        self.read_d_bits(index)
    }
    #[inline]
    pub fn write_bits(&mut self, index: usize, bits: u64) {
        self.write_d_bits(index, bits)
    }

    // ===== FCSR helpers (FCC0 en bit 23) =====
    #[inline]
    pub fn set_fcc0(&mut self, cond: bool) {
        const FCC0_BIT: u32 = 1 << 23;
        if cond { self.fcsr |= FCC0_BIT; } else { self.fcsr &= !FCC0_BIT; }
    }
    #[inline]
    pub fn get_fcc0(&self) -> bool {
        (self.fcsr >> 23) & 1 != 0
    }

    pub fn dump(&self) {
        println!("--- COP1 (FPU) Registers ---");
        for (i, bits) in self.regs.iter().enumerate() {
            let d = f64::from_bits(*bits);
            println!("F{:02} = {:>20.10}  (0x{:016X})", i, d, bits);
        }
        println!("FCSR = 0x{:08X}", self.fcsr);
        println!("FIR  = 0x{:08X}", self.fir);
    }
}

