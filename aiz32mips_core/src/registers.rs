#[derive(Clone, Copy, Debug)]
pub struct Registers {
    pub general: [u32; 32],
    pub special: SpecialRegisters,
}

#[derive(Clone, Copy, Debug)]
pub struct SpecialRegisters {
    pub hi: u32,
    pub lo: u32,
    pub pc: u32,
    pub epc: u32,
    pub status: u32,
    pub cause: u32,
    pub badvaddr: u32,
    pub index: u32,
    pub random: u32,
    pub entrylo0: u32,
    pub entrylo1: u32,
    pub count: u32,
    pub compare: u32,
    pub entryhi: u32,
}

impl Default for Registers {
    fn default() -> Self {
        Self {
            general: [0; 32],
            special: SpecialRegisters::default(),
        }
    }
}

impl Default for SpecialRegisters {
    fn default() -> Self {
        Self {
            hi: 0,
            lo: 0,
            pc: 0xBFC00000,
            epc: 0,
            status: 0x00000000,
            cause: 0,
            badvaddr: 0,
            index: 0,
            random: 31,
            entrylo0: 0,
            entrylo1: 0,
            count: 0,
            compare: 0,
            entryhi: 0,
        }
    }
}

impl Registers {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn get_pc(&self) -> u32 {
        self.special.pc
    }

    pub fn set_pc(&mut self, value: u32) {
        self.special.pc = value;
    }

    pub fn get_gpc(&self) -> u32 {
        self.general[28]
    }

    pub fn set_gpc(&mut self, value: u32) {
        self.general[28] = value;
    }

    pub fn get_sp(&self) -> u32 {
        self.general[29]
    }

    pub fn set_sp(&mut self, value: u32) {
        self.general[29] = value;
    }

    pub fn get_fp(&self) -> u32 {
        self.general[30]
    }

    pub fn set_fp(&mut self, value: u32) {
        self.general[30] = value;
    }

    pub fn read(&self, index: usize) -> u32 {
        if index == 0 { 0 } else { self.general[index] }
    }

    pub fn write(&mut self, index: usize, value: u32) {
        if index != 0 && index < 32 {
            self.general[index] = value;
        }
    }

    pub fn read_special(&self, name: SpecialReg) -> u32 {
        match name {
            SpecialReg::Hi => self.special.hi,
            SpecialReg::Lo => self.special.lo,
            SpecialReg::Pc => self.special.pc,
            SpecialReg::Epc => self.special.epc,
            SpecialReg::Status => self.special.status,
            SpecialReg::Cause => self.special.cause,
            SpecialReg::BadVAddr => self.special.badvaddr,
            SpecialReg::Index => self.special.index,
            SpecialReg::Random => self.special.random,
            SpecialReg::EntryLo0 => self.special.entrylo0,
            SpecialReg::EntryLo1 => self.special.entrylo1,
            SpecialReg::Count => self.special.count,
            SpecialReg::Compare => self.special.compare,
            SpecialReg::EntryHi => self.special.entryhi,
        }
    }

    pub fn write_special(&mut self, name: SpecialReg, value: u32) {
        match name {
            SpecialReg::Hi => self.special.hi = value,
            SpecialReg::Lo => self.special.lo = value,
            SpecialReg::Pc => self.special.pc = value,
            SpecialReg::Epc => self.special.epc = value,
            SpecialReg::Status => self.special.status = value,
            SpecialReg::Cause => self.special.cause = value,
            SpecialReg::BadVAddr => self.special.badvaddr = value,
            SpecialReg::Index => self.special.index = value,
            SpecialReg::Random => self.special.random = value,
            SpecialReg::EntryLo0 => self.special.entrylo0 = value,
            SpecialReg::EntryLo1 => self.special.entrylo1 = value,
            SpecialReg::Count => self.special.count = value,
            SpecialReg::Compare => self.special.compare = value,
            SpecialReg::EntryHi => self.special.entryhi = value,
        }
    }

    pub fn tick(&mut self) {
        self.special.count = self.special.count.wrapping_add(1);
    }

    pub fn dump(&self) {
        println!("--- CPU Registers ---");
        for (i, reg) in self.general.iter().enumerate() {
            println!("r{:02} = 0x{:08X}", i, reg);
        }
        println!("HI = 0x{:08X}", self.special.hi);
        println!("LO = 0x{:08X}", self.special.lo);
        println!("PC = 0x{:08X}", self.special.pc);
        println!("EPC = 0x{:08X}", self.special.epc);
        println!("Status = 0x{:08X}", self.special.status);
        println!("Cause  = 0x{:08X}", self.special.cause);
    }
}

#[derive(Clone, Copy, Debug)]
pub enum SpecialReg {
    Hi,
    Lo,
    Pc,
    Epc,
    Status,
    Cause,
    BadVAddr,
    Index,
    Random,
    EntryLo0,
    EntryLo1,
    Count,
    Compare,
    EntryHi,
}
