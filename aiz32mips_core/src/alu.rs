#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AluOp {
    Add,
    Sub,
    And,
    Or,
    Xor,
    Nor,
    Slt,
    Sltu,
    Sll,
    Srl,
    Sra,
    Mult,
    Multu,
    Div,
    Divu,
    Mfhi,
    Mflo,
    Mthi,
    Mtlo,
    Lui,
    Teq,
    None,
}

#[derive(Debug, Clone, Copy)]
pub struct AluResult {
    pub value: u32,
    pub hi: Option<u32>,
    pub lo: Option<u32>,
    pub overflow: bool,
    pub op: AluOp,
}

pub struct ALU;

impl ALU {
    pub fn new() -> Self {
        Self {}
    }

    pub fn execute(
        rs_val: u32,
        rt_val: u32,
        shamt: u8,
        imm: u16,
        opcode: u8,
        funct: u8,
        inmediate: bool,
        hi: u32,
        lo: u32,
    ) -> AluResult {
        let overflow = false;
        let mut result = 0;
        let mut hi_res = None;
        let mut lo_res = None;
        let mut op = AluOp::None;

        // === I-TYPE ===
        if inmediate {
            let imm_se = imm as i16 as i32 as u32;
            
            match opcode {
                0x08 => {
                    // ADDI
                    result = rs_val.wrapping_add(imm_se);
                    op = AluOp::Add;
                }
                0x09 => {
                    // ADDIU
                    result = rs_val.wrapping_add(imm_se);
                    op = AluOp::Add;
                }
                0x0C => {
                    // ANDI
                    result = rs_val & (imm as u32);
                    op = AluOp::And;
                }
                0x0D => {
                    // ORI
                    result = rs_val | (imm as u32);
                    op = AluOp::Or;
                }
                0x0E => {
                    // XORI
                    result = rs_val ^ (imm as u32);
                    op = AluOp::Xor;
                }
                0x0A => {
                    // SLTI
                    result = if (rs_val as i32) < (imm as i16 as i32) {
                        1
                    } else {
                        0
                    };
                    op = AluOp::Slt;
                }
                0x0B => {
                    // SLTIU
                    result = if rs_val < (imm as u32) { 1 } else { 0 };
                    op = AluOp::Sltu;
                }
                0x0F => {
                    // LUI
                    result = (imm as u32) << 16;
                    op = AluOp::Lui;
                }
                _ => println!("[ALU] Unhandled I-type opcode 0x{:02X}", opcode),
            }
        } else {
            // === R-TYPE ===
            match funct {
                // ---- Aritméticas ----
                0x20 => {
                    result = rs_val.wrapping_add(rt_val);
                    op = AluOp::Add;
                } // ADD
                0x21 => {
                    result = rs_val.wrapping_add(rt_val);
                    op = AluOp::Add;
                } // ADDU
                0x22 => {
                    result = rs_val.wrapping_sub(rt_val);
                    op = AluOp::Sub;
                } // SUB
                0x23 => {
                    result = rs_val.wrapping_sub(rt_val);
                    op = AluOp::Sub;
                } // SUBU
                // ---- Lógicas ----
                0x24 => {
                    result = rs_val & rt_val;
                    op = AluOp::And;
                } // AND
                0x25 => {
                    result = rs_val | rt_val;
                    op = AluOp::Or;
                } // OR
                0x26 => {
                    result = rs_val ^ rt_val;
                    op = AluOp::Xor;
                } // XOR
                0x27 => {
                    result = !(rs_val | rt_val);
                    op = AluOp::Nor;
                } // NOR
                // ---- Comparaciones ----
                0x2A => {
                    result = if (rs_val as i32) < (rt_val as i32) {
                        1
                    } else {
                        0
                    };
                    op = AluOp::Slt;
                }
                0x2B => {
                    result = if rs_val < rt_val { 1 } else { 0 };
                    op = AluOp::Sltu;
                }
                0x34 => {
                    // TEQ
                    if rs_val == rt_val {
                        result = 1;
                    }
                    op = AluOp::Teq;
                }
                // ---- Desplazamientos ----
                0x00 => {
                    result = rt_val << shamt;
                    op = AluOp::Sll;
                } // SLL
                0x02 => {
                    result = rt_val >> shamt;
                    op = AluOp::Srl;
                } // SRL
                0x03 => {
                    result = ((rt_val as i32) >> shamt) as u32;
                    op = AluOp::Sra;
                } // SRA
                0x04 => {
                    result = rt_val << (rs_val & 0x1F);
                    op = AluOp::Sll;
                } // SLLV
                0x06 => {
                    result = rt_val >> (rs_val & 0x1F);
                    op = AluOp::Srl;
                } // SRLV
                0x07 => {
                    result = ((rt_val as i32) >> (rs_val & 0x1F)) as u32;
                    op = AluOp::Sra;
                } // SRAV
                // ---- Multiplicación / División ----
                0x18 => {
                    // MULT
                    let prod = (rs_val as i32 as i64) * (rt_val as i32 as i64);
                    hi_res = Some((prod >> 32) as u32);
                    lo_res = Some(prod as u32);
                    op = AluOp::Mult;
                }
                0x19 => {
                    // MULTU
                    let prod = (rs_val as u64) * (rt_val as u64);
                    hi_res = Some((prod >> 32) as u32);
                    lo_res = Some(prod as u32);
                    op = AluOp::Multu;
                }
                0x1A => {
                    // DIV
                    if rt_val != 0 {
                        hi_res = Some((rs_val as i32 % rt_val as i32) as u32);
                        lo_res = Some((rs_val as i32 / rt_val as i32) as u32);
                    }
                    op = AluOp::Div;
                }
                0x1B => {
                    // DIVU
                    if rt_val != 0 {
                        hi_res = Some(rs_val % rt_val);
                        lo_res = Some(rs_val / rt_val);
                    }
                    op = AluOp::Divu;
                }
                0x10 => {
                    result = hi;
                    op = AluOp::Mfhi;
                }
                0x12 => {
                    result = lo;
                    op = AluOp::Mflo;
                }
                0x11 => {
                    hi_res = Some(rs_val);
                    op = AluOp::Mthi;
                }
                0x13 => {
                    lo_res = Some(rs_val);
                    op = AluOp::Mtlo;
                }
                0x08 |
                0x09 |
                0x0C |
                0x0D => {}
                _ => println!("[ALU] Unhandled R-type funct 0x{:02X}", funct),
            }
        }

        AluResult {
            value: result,
            hi: hi_res,
            lo: lo_res,
            overflow,
            op,
        }
    }
}
