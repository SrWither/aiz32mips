/// Códigos de excepción MIPS32 (campo ExcCode de CP0 Cause, bits 2-6).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExcCode {
    Int = 0,  // Interrupt
    Mod = 1,  // TLB modification (escritura sin permiso)
    TlbL = 2, // TLB miss en load/fetch
    TlbS = 3, // TLB miss en store
    AdEL = 4, // Address error, load/fetch
    AdES = 5, // Address error, store
    IBE = 6,  // Instruction bus error
    DBE = 7,  // Data bus error
    Sys = 8,  // Syscall
    Bp = 9,   // Breakpoint
    RI = 10,  // Reserved instruction
    CpU = 11, // Coprocessor unusable
    Ov = 12,  // Arithmetic overflow
    Tr = 13,  // Trap
    FPE = 15, // Floating point exception
}

impl ExcCode {
    #[inline]
    pub fn code(self) -> u32 {
        self as u32
    }
}
