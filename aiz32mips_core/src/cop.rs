pub struct Cop0 {
    pub regs: [u32; 32],
}

pub struct Cop1 {
    pub f: [f64; 32],
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
            f: [0.0; 32],
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
}

impl Cop1 {
    pub fn new() -> Self {
        Self::default()
    }

    #[inline]
    pub fn read_f(&self, index: usize) -> f64 {
        self.f[index]
    }

    #[inline]
    pub fn write_f(&mut self, index: usize, value: f64) {
        self.f[index] = value;
    }

    #[inline]
    pub fn read_bits(&self, index: usize) -> u64 {
        self.f[index].to_bits()
    }

    #[inline]
    pub fn write_bits(&mut self, index: usize, bits: u64) {
        self.f[index] = f64::from_bits(bits);
    }

    #[inline]
    pub fn fcsr(&self) -> u32 {
        self.fcsr
    }

    #[inline]
    pub fn set_fcsr(&mut self, value: u32) {
        self.fcsr = value;
    }

    #[inline]
    pub fn fir(&self) -> u32 {
        self.fir
    }

    pub fn dump(&self) {
        println!("--- COP1 (FPU) Registers ---");
        for (i, r) in self.f.iter().enumerate() {
            println!("F{:02} = {:>20.10}", i, r);
        }
        println!("FCSR = 0x{:08X}", self.fcsr);
        println!("FIR  = 0x{:08X}", self.fir);
    }
}

