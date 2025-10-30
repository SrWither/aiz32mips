use crate::alu::*;
use crate::cop::*;
use crate::instructions::*;
use crate::memory::*;
use crate::registers::*;

pub struct CPU {
    pub registers: Registers,
    pub cop0: Cop0,
    pub cop1: Cop1,
    pub alu: ALU,
}

impl CPU {
    pub fn new() -> Self {
        let mut cpu = Self {
            registers: Registers::default(),
            cop0: Cop0::default(),
            cop1: Cop1::default(),
            alu: ALU,
        };
        cpu.reset();
        cpu
    }

    pub fn reset(&mut self) {
        self.registers.reset();
        self.cop0 = Cop0::default();
        self.cop1 = Cop1::default();

        let sp = 0x8000_0000 + 0x0010_0000 - 0x1000;
        self.registers.set_sp(sp);
        self.registers.set_gpc(0xBFC0_0000);
        self.registers.write(31, 0xBFC0_0000);

        println!(
            "[RESET] SP={:#010X}  PC={:#010X}",
            self.registers.get_sp(),
            self.registers.get_pc()
        );
    }

    pub fn step(&mut self, bus: &mut MemoryBus) {
        let instr_word = match self.fetch(bus) {
            Some(v) => v,
            None => return,
        };

        let decoded = self.decode(instr_word);
        let exec_result = self.execute(bus, decoded);
        self.writeback(exec_result);
    }

    pub fn fetch(&mut self, bus: &mut MemoryBus) -> Option<u32> {
        let pc = self.registers.get_pc();
        match bus.read32_virt(pc) {
            Ok(instr) => {
                self.registers.special.pc = pc.wrapping_add(4);
                Some(instr)
            }
            Err(e) => {
                println!("Fetch error at PC {:#010X}: {:?}", pc, e);
                None
            }
        }
    }

    pub fn decode(&self, instr: u32) -> Instruction {
        let decoded = Instruction::decode(instr);
        decoded
    }

    pub fn execute(&mut self, bus: &mut MemoryBus, instr: Instruction) -> u32 {
        match instr {
            Instruction::RType(r) => {
                let rs_val = self.registers.read(r.rs as usize);
                let rt_val = self.registers.read(r.rt as usize);
                let shamt = r.shamt;

                // Control flow ops (JR / JALR)
                match r.funct {
                    0x08 => {
                        // JR
                        let target = rs_val;
                        if let Some(v) = self.fetch(bus) {
                            let d = self.decode(v);
                            self.execute(bus, d);
                        }
                        self.registers.special.pc = target;
                        return 0;
                    }
                    0x09 => {
                        // JALR
                        let link = self.registers.get_pc().wrapping_add(4); // PC + 8
                        let target = rs_val;
                        if r.rd != 0 {
                            self.registers.write(r.rd as usize, link);
                        }
                        if let Some(v) = self.fetch(bus) {
                            let d = self.decode(v);
                            self.execute(bus, d);
                        }
                        self.registers.special.pc = target;
                        return 0;
                    }

                    _ => {}
                }

                // Regular ALU execution
                let res = ALU::execute(
                    rs_val,
                    rt_val,
                    shamt,
                    0,
                    r.opcode,
                    r.funct,
                    false,
                    self.registers.special.hi,
                    self.registers.special.lo,
                );

                if let Some(hi) = res.hi {
                    self.registers.special.hi = hi;
                }
                if let Some(lo) = res.lo {
                    self.registers.special.lo = lo;
                }

                if r.rd != 0 {
                    self.registers.write(r.rd as usize, res.value);
                }

                res.value
            }

            Instruction::IType(i) => {
                let rs_val = self.registers.read(i.rs as usize);
                let rt_val = self.registers.read(i.rt as usize);
                let imm_signed = i.imm as i16 as i32;
                let imm_u = imm_signed as u32;
                let pc_next = self.registers.get_pc();

                // === Branch instructions ===
                match i.opcode {
                    0x04 => {
                        // BEQ
                        if rs_val == rt_val {
                            let offset = ((i.imm as i16 as i32) << 2) as u32;
                            let delay = self.fetch(bus);
                            if let Some(v) = delay {
                                let delay_decoded = self.decode(v);
                                self.execute(bus, delay_decoded);
                            }
                            self.registers.special.pc = pc_next.wrapping_add(offset);
                        }
                        return 0;
                    }
                    0x05 => {
                        // BNE
                        if rs_val != rt_val {
                            let offset = ((i.imm as i16 as i32) << 2) as u32;
                            let delay = self.fetch(bus);
                            if let Some(v) = delay {
                                let delay_decoded = self.decode(v);
                                self.execute(bus, delay_decoded);
                            }
                            self.registers.special.pc = pc_next.wrapping_add(offset);
                        }
                        return 0;
                    }
                    0x06 => {
                        // BLEZ
                        if (rs_val as i32) <= 0 {
                            let offset = ((i.imm as i16 as i32) << 2) as u32;
                            let delay = self.fetch(bus);
                            if let Some(v) = delay {
                                let delay_decoded = self.decode(v);
                                self.execute(bus, delay_decoded);
                            }
                            self.registers.special.pc = pc_next.wrapping_add(offset);
                        }
                        return 0;
                    }
                    0x07 => {
                        // BGTZ
                        if (rs_val as i32) > 0 {
                            let offset = ((i.imm as i16 as i32) << 2) as u32;
                            let delay = self.fetch(bus);
                            if let Some(v) = delay {
                                let delay_decoded = self.decode(v);
                                self.execute(bus, delay_decoded);
                            }
                            self.registers.special.pc = pc_next.wrapping_add(offset);
                        }
                        return 0;
                    }
                    0x01 => {
                        let rt = i.rt;
                        let (cond, link) = match rt {
                            0x00 => ((rs_val as i32) < 0, false),  // BLTZ
                            0x01 => ((rs_val as i32) >= 0, false), // BGEZ
                            0x10 => ((rs_val as i32) < 0, true),   // BLTZAL
                            0x11 => ((rs_val as i32) >= 0, true),  // BGEZAL
                            _ => (false, false),
                        };

                        // delay slot SIEMPRE
                        if let Some(v) = self.fetch(bus) {
                            let d = self.decode(v);
                            self.execute(bus, d);
                        }

                        if cond {
                            if link {
                                self.registers
                                    .write(31, self.registers.get_pc().wrapping_add(4));
                            } // PC+8
                            let branch_target = self
                                .registers
                                .get_pc()
                                .wrapping_add(((i.imm as i16 as i32) << 2) as u32);
                            self.registers.special.pc = branch_target;
                        }
                        return 0;
                    }

                    0x23 => {
                        // LW rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        if let Ok(val) = bus.read32_virt(addr) {
                            if i.rt != 0 {
                                self.registers.write(i.rt as usize, val);
                            }
                            return val;
                        }
                        return 0;
                    }
                    0x2B => {
                        // SW rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = rt_val;
                        bus.write32_virt(addr, val).ok();
                        return 0;
                    }
                    0x20 => {
                        // LB rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = bus.read8_virt(addr).unwrap_or(0) as i8 as i32 as u32;
                        if i.rt != 0 {
                            self.registers.write(i.rt as usize, val);
                        }
                        return val;
                    }
                    0x24 => {
                        // LBU rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = bus.read8_virt(addr).unwrap_or(0) as u32;
                        if i.rt != 0 {
                            self.registers.write(i.rt as usize, val);
                        }
                        return val;
                    }
                    0x21 => {
                        // LH rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = bus.read16(addr).unwrap_or(0) as i16 as i32 as u32;
                        if i.rt != 0 {
                            self.registers.write(i.rt as usize, val);
                        }
                        return val;
                    }
                    0x25 => {
                        // LHU rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = bus.read16(addr).unwrap_or(0) as u32;
                        if i.rt != 0 {
                            self.registers.write(i.rt as usize, val);
                        }
                        return val;
                    }
                    0x28 => {
                        // SB rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = (rt_val & 0xFF) as u8;
                        bus.write8_virt(addr, val).ok();
                        return 0;
                    }
                    0x29 => {
                        // SH rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = (rt_val & 0xFFFF) as u16;
                        bus.write16(addr, val).ok();
                        return 0;
                    }
                    _ => {}
                }

                // Normal I-Type ALU ops

                // println!("Inmediate: 0x{:04X} (signed: {})", i.imm, imm_signed);
                let res = ALU::execute(
                    rs_val,
                    rt_val,
                    0,
                    i.imm,
                    i.opcode,
                    0,
                    true,
                    self.registers.special.hi,
                    self.registers.special.lo,
                );

                if i.rt != 0 {
                    self.registers.write(i.rt as usize, res.value);
                }

                res.value
            }

            Instruction::Special2(s) => match s.funct {
                0x00 => {
                    // MADD (signed)
                    let rs_val = self.registers.read(s.rs as usize) as i32 as i64;
                    let rt_val = self.registers.read(s.rt as usize) as i32 as i64;
                    let prod = rs_val.wrapping_mul(rt_val);
                    let acc = ((self.registers.special.hi as i64) << 32)
                        | (self.registers.special.lo as i64);
                    let res = acc.wrapping_add(prod);
                    self.registers.special.hi = (res >> 32) as u32;
                    self.registers.special.lo = (res & 0xFFFF_FFFF) as u32;
                    0
                }

                0x01 => {
                    // MADDU (unsigned)
                    let rs_val = self.registers.read(s.rs as usize) as u64;
                    let rt_val = self.registers.read(s.rt as usize) as u64;
                    let prod = rs_val.wrapping_mul(rt_val);
                    let acc = ((self.registers.special.hi as u64) << 32)
                        | (self.registers.special.lo as u64);
                    let res = acc.wrapping_add(prod);
                    self.registers.special.hi = (res >> 32) as u32;
                    self.registers.special.lo = (res & 0xFFFF_FFFF) as u32;
                    0
                }

                0x02 => {
                    // MUL (signed)
                    let rs_val = self.registers.read(s.rs as usize) as i32 as i64;
                    let rt_val = self.registers.read(s.rt as usize) as i32 as i64;
                    let prod = rs_val.wrapping_mul(rt_val);
                    let rd = s.rd as usize;
                    self.registers.write(rd, prod as u32);
                    self.registers.special.hi = (prod >> 32) as u32;
                    self.registers.special.lo = (prod & 0xFFFF_FFFF) as u32;
                    0
                }

                0x04 => {
                    // MSUB (signed)
                    let rs_val = self.registers.read(s.rs as usize) as i32 as i64;
                    let rt_val = self.registers.read(s.rt as usize) as i32 as i64;
                    let prod = rs_val.wrapping_mul(rt_val);
                    let acc = ((self.registers.special.hi as i64) << 32)
                        | (self.registers.special.lo as i64);
                    let res = acc.wrapping_sub(prod);
                    self.registers.special.hi = (res >> 32) as u32;
                    self.registers.special.lo = (res & 0xFFFF_FFFF) as u32;
                    0
                }

                0x05 => {
                    // MSUBU (unsigned)
                    let rs_val = self.registers.read(s.rs as usize) as u64;
                    let rt_val = self.registers.read(s.rt as usize) as u64;
                    let prod = rs_val.wrapping_mul(rt_val);
                    let acc = ((self.registers.special.hi as u64) << 32)
                        | (self.registers.special.lo as u64);
                    let res = acc.wrapping_sub(prod);
                    self.registers.special.hi = (res >> 32) as u32;
                    self.registers.special.lo = (res & 0xFFFF_FFFF) as u32;
                    0
                }

                _ => {
                    println!(
                        "[SPECIAL2] Unhandled funct=0x{:02X} at PC={:#010X}",
                        s.funct,
                        self.registers.get_pc()
                    );
                    0
                }
            },

            Instruction::JType(j) => {
                let pc_next = self.registers.get_pc();
                let target = (pc_next & 0xF000_0000) | (j.target << 2);

                if j.opcode == 0x03 {
                    self.registers.write(31, pc_next.wrapping_add(4));
                }

                if let Some(v) = self.fetch(bus) {
                    let delay_decoded = self.decode(v);
                    self.execute(bus, delay_decoded);
                }

                self.registers.special.pc = target;
                0
            }

            _ => 0,
        }
    }

    pub fn memory_access(&mut self, exec_result: u32) -> u32 {
        exec_result
    }

    pub fn writeback(&mut self, _mem_result: u32) {}
}
