#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Instruction {
    RType(RType),
    IType(IType),
    JType(JType),
    Cop0(Cop0Ins),
    Cop1(Cop1Ins),
    Special2(Special2),
    Special3(Special3),
    Invalid(u32),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RType {
    pub opcode: u8, // 6 bits
    pub rs: u8,     // 5 bits
    pub rt: u8,     // 5 bits
    pub rd: u8,     // 5 bits
    pub shamt: u8,  // 5 bits
    pub funct: u8,  // 6 bits
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IType {
    pub opcode: u8, // 6 bits
    pub rs: u8,     // 5 bits
    pub rt: u8,     // 5 bits
    pub imm: u16,   // 16 bits
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct JType {
    pub opcode: u8,  // 6 bits
    pub target: u32, // 26 bits
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cop0Ins {
    pub opcode: u8, // 6 bits (010000)
    pub rs: u8,     // 5 bits
    pub rt: u8,     // 5 bits
    pub rd: u8,     // 5 bits
    pub sel: u8,    // 3 bits
    pub funct: u8,  // 6 bits
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cop1Ins {
    pub opcode: u8, // 6 bits (010001)
    pub fmt: u8,    // 5 bits
    pub ft: u8,     // 5 bits
    pub fs: u8,     // 5 bits
    pub fd: u8,     // 5 bits
    pub funct: u8,  // 6 bits
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Special2 {
    pub opcode: u8, // 6 bits (011100)
    pub rs: u8,
    pub rt: u8,
    pub rd: u8,
    pub shamt: u8,
    pub funct: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Special3 {
    pub opcode: u8, // 6 bits (011111)
    pub rs: u8,
    pub rt: u8,
    pub rd: u8,
    pub sa: u8,
    pub funct: u8,
}

impl Instruction {
    pub fn decode(instr: u32) -> Self {
        let opcode = ((instr >> 26) & 0x3F) as u8;
        match opcode {
            0x00 => Instruction::RType(RType::decode(instr)), // SPECIAL
            0x10 => Instruction::Cop0(Cop0Ins::decode(instr)),   // COP0
            0x11 => Instruction::Cop1(Cop1Ins::decode(instr)),   // COP1
            0x1C => Instruction::Special2(Special2::decode(instr)), // SPECIAL2
            0x1F => Instruction::Special3(Special3::decode(instr)), // SPECIAL3
            0x02 | 0x03 => Instruction::JType(JType::decode(instr)), // J, JAL
            _ => Instruction::IType(IType::decode(instr)),
        }
    }

    pub fn encode(&self) -> u32 {
        match *self {
            Instruction::RType(r) => r.encode(),
            Instruction::IType(i) => i.encode(),
            Instruction::JType(j) => j.encode(),
            Instruction::Cop0(c) => c.encode(),
            Instruction::Cop1(c) => c.encode(),
            Instruction::Special2(s) => s.encode(),
            Instruction::Special3(s) => s.encode(),
            Instruction::Invalid(val) => val,
        }
    }
}

impl RType {
    pub fn decode(instr: u32) -> Self {
        Self {
            opcode: ((instr >> 26) & 0x3F) as u8,
            rs: ((instr >> 21) & 0x1F) as u8,
            rt: ((instr >> 16) & 0x1F) as u8,
            rd: ((instr >> 11) & 0x1F) as u8,
            shamt: ((instr >> 6) & 0x1F) as u8,
            funct: (instr & 0x3F) as u8,
        }
    }

    pub fn encode(&self) -> u32 {
        ((self.opcode as u32) << 26)
            | ((self.rs as u32) << 21)
            | ((self.rt as u32) << 16)
            | ((self.rd as u32) << 11)
            | ((self.shamt as u32) << 6)
            | (self.funct as u32)
    }
}

impl IType {
    pub fn decode(instr: u32) -> Self {
        Self {
            opcode: ((instr >> 26) & 0x3F) as u8,
            rs: ((instr >> 21) & 0x1F) as u8,
            rt: ((instr >> 16) & 0x1F) as u8,
            imm: (instr & 0xFFFF) as u16,
        }
    }

    pub fn encode(&self) -> u32 {
        ((self.opcode as u32) << 26)
            | ((self.rs as u32) << 21)
            | ((self.rt as u32) << 16)
            | (self.imm as u32)
    }
}

impl JType {
    pub fn decode(instr: u32) -> Self {
        Self {
            opcode: ((instr >> 26) & 0x3F) as u8,
            target: instr & 0x03FF_FFFF,
        }
    }

    pub fn encode(&self) -> u32 {
        ((self.opcode as u32) << 26) | (self.target & 0x03FF_FFFF)
    }
}

impl Cop0Ins {
    pub fn decode(instr: u32) -> Self {
        Self {
            opcode: ((instr >> 26) & 0x3F) as u8,
            rs: ((instr >> 21) & 0x1F) as u8,
            rt: ((instr >> 16) & 0x1F) as u8,
            rd: ((instr >> 11) & 0x1F) as u8,
            sel: (instr & 0x7) as u8,
            funct: (instr & 0x3F) as u8,
        }
    }

    pub fn encode(&self) -> u32 {
        ((self.opcode as u32) << 26)
            | ((self.rs as u32) << 21)
            | ((self.rt as u32) << 16)
            | ((self.rd as u32) << 11)
            | (self.funct as u32)
    }
}

impl Cop1Ins {
    pub fn decode(instr: u32) -> Self {
        Self {
            opcode: ((instr >> 26) & 0x3F) as u8,
            fmt: ((instr >> 21) & 0x1F) as u8,
            ft: ((instr >> 16) & 0x1F) as u8,
            fs: ((instr >> 11) & 0x1F) as u8,
            fd: ((instr >> 6) & 0x1F) as u8,
            funct: (instr & 0x3F) as u8,
        }
    }

    pub fn encode(&self) -> u32 {
        ((self.opcode as u32) << 26)
            | ((self.fmt as u32) << 21)
            | ((self.ft as u32) << 16)
            | ((self.fs as u32) << 11)
            | ((self.fd as u32) << 6)
            | (self.funct as u32)
    }
}

impl Special2 {
    pub fn decode(instr: u32) -> Self {
        Self {
            opcode: ((instr >> 26) & 0x3F) as u8,
            rs: ((instr >> 21) & 0x1F) as u8,
            rt: ((instr >> 16) & 0x1F) as u8,
            rd: ((instr >> 11) & 0x1F) as u8,
            shamt: ((instr >> 6) & 0x1F) as u8,
            funct: (instr & 0x3F) as u8,
        }
    }

    pub fn encode(&self) -> u32 {
        ((self.opcode as u32) << 26)
            | ((self.rs as u32) << 21)
            | ((self.rt as u32) << 16)
            | ((self.rd as u32) << 11)
            | ((self.shamt as u32) << 6)
            | (self.funct as u32)
    }
}


impl Special3 {
    pub fn decode(instr: u32) -> Self {
        Self {
            opcode: ((instr >> 26) & 0x3F) as u8,
            rs: ((instr >> 21) & 0x1F) as u8,
            rt: ((instr >> 16) & 0x1F) as u8,
            rd: ((instr >> 11) & 0x1F) as u8,
            sa: ((instr >> 6) & 0x1F) as u8,
            funct: (instr & 0x3F) as u8,
        }
    }

    pub fn encode(&self) -> u32 {
        ((self.opcode as u32) << 26)
            | ((self.rs as u32) << 21)
            | ((self.rt as u32) << 16)
            | ((self.rd as u32) << 11)
            | ((self.sa as u32) << 6)
            | (self.funct as u32)
    }
}
