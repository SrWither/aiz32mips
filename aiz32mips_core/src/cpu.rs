use crate::alu::*;
use crate::cop::*;
use crate::exception::ExcCode;
use crate::instructions::*;
use crate::memory::*;
use crate::registers::*;
use crate::tlb::{Tlb, TlbEntry, TLB_ENTRIES};

pub struct CPU {
    pub registers: Registers,
    pub cop0: Cop0,
    pub cop1: Cop1,
    pub alu: ALU,
    pub tlb: Tlb,
    /// Se pone en true cuando una excepción redirige el PC durante el step
    /// actual; evita que el código que sigue (p.ej. el branch que disparó
    /// el delay slot) pise el PC del vector de excepción.
    exception_taken: bool,
}

impl CPU {
    pub fn new() -> Self {
        let mut cpu = Self {
            registers: Registers::default(),
            cop0: Cop0::default(),
            cop1: Cop1::default(),
            alu: ALU,
            tlb: Tlb::default(),
            exception_taken: false,
        };
        cpu.reset();
        cpu
    }

    #[inline]
    fn set_fcc0(&mut self, cond: bool) {
        if cond {
            self.cop1.fcsr |= 1 << 23;
        } else {
            self.cop1.fcsr &= !(1 << 23);
        }
    }

    #[inline]
    fn get_fcc0(&self) -> bool {
        (self.cop1.fcsr >> 23) & 1 != 0
    }

    pub fn reset(&mut self) {
        self.registers.reset();
        self.cop0 = Cop0::default();
        self.cop1 = Cop1::default();
        self.tlb = Tlb::default();
        self.exception_taken = false;

        // En hardware real, tras el reset BEV=1 (vectores de excepción en
        // la ROM de arranque, kseg1) y ERL=1 (modo error level).
        self.cop0.set_status(STATUS_BEV | STATUS_ERL);

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
        self.exception_taken = false;
        self.cop0.tick_timer();

        // Las interrupciones se toman en el límite de instrucción, antes de
        // hacer fetch de la próxima. Como los delay slots se resuelven
        // íntegramente dentro de un mismo step(), PC siempre apunta acá a
        // una instrucción "normal" => BD=0 es correcto.
        if self.cop0.interrupts_enabled() && self.cop0.pending_enabled_interrupts() != 0 {
            let pc = self.registers.get_pc();
            self.raise_exception(ExcCode::Int, pc, false, None);
            return;
        }

        let instr_pc = self.registers.get_pc();
        let instr_word = match self.fetch(bus, instr_pc, false) {
            Some(v) => v,
            None => return,
        };

        let decoded = self.decode(instr_word);
        let exec_result = self.execute(bus, decoded, instr_pc, false);
        self.writeback(exec_result);
    }

    /// Dispara una excepción síncrona: guarda EPC/BD, fija ExcCode y
    /// BadVAddr, levanta Status.EXL y redirige el PC al vector general.
    /// `instr_pc` es la dirección de la instrucción que causó la excepción
    /// si `in_delay_slot` es false, o la dirección del branch/jump dueño
    /// del delay slot si es true (así EPC siempre apunta donde corresponde).
    pub fn raise_exception(
        &mut self,
        exc: ExcCode,
        instr_pc: u32,
        in_delay_slot: bool,
        bad_vaddr: Option<u32>,
    ) {
        // Si ya estábamos en una excepción (EXL=1), EPC/BD no se tocan:
        // la excepción anidada hereda el contexto de la original.
        if !self.cop0.status_bit(STATUS_EXL) {
            self.cop0.set_epc(instr_pc);
            self.cop0.set_cause_bd(in_delay_slot);
        }

        self.cop0.set_cause_exc_code(exc.code());
        if let Some(va) = bad_vaddr {
            self.cop0.set_badvaddr(va);
        }
        self.cop0.set_status_bit(STATUS_EXL);

        self.registers.special.pc = self.exception_vector(false);
        self.exception_taken = true;
    }

    fn exception_vector(&self, refill: bool) -> u32 {
        if refill {
            if self.cop0.status_bit(STATUS_BEV) {
                0xBFC0_0200
            } else {
                0x8000_0000
            }
        } else if self.cop0.status_bit(STATUS_BEV) {
            0xBFC0_0180
        } else {
            0x8000_0180
        }
    }

    fn raise_addr_error(&mut self, is_store: bool, vaddr: u32, instr_pc: u32, in_delay_slot: bool) {
        let exc = if is_store { ExcCode::AdES } else { ExcCode::AdEL };
        self.raise_exception(exc, instr_pc, in_delay_slot, Some(vaddr));
    }

    /// Excepción de TLB (refill miss / invalid / modified): además de lo que
    /// hace raise_exception, actualiza Context.BadVPN2 como en hardware real
    /// (para que un refill handler clásico de 2 instrucciones funcione).
    /// `is_refill_miss` distingue un miss real (sin ninguna entrada que
    /// matchee) -> vector de refill, de un hit con entrada inválida/sin
    /// permiso de escritura -> vector general.
    fn raise_tlb_exception(
        &mut self,
        exc: ExcCode,
        instr_pc: u32,
        in_delay_slot: bool,
        vaddr: u32,
        is_refill_miss: bool,
    ) {
        let ctx = self.cop0.read(CP0_REG_CONTEXT);
        self.cop0
            .write(CP0_REG_CONTEXT, (ctx & 0xFF80_0000) | ((vaddr >> 13) << 4));

        // El vector de refill solo se usa si todavía no estábamos en una
        // excepción (EXL=0); hay que leerlo antes de levantar EXL abajo.
        let use_refill = is_refill_miss && !self.cop0.status_bit(STATUS_EXL);

        if !self.cop0.status_bit(STATUS_EXL) {
            self.cop0.set_epc(instr_pc);
            self.cop0.set_cause_bd(in_delay_slot);
        }
        self.cop0.set_cause_exc_code(exc.code());
        self.cop0.set_badvaddr(vaddr);
        self.cop0.set_status_bit(STATUS_EXL);

        self.registers.special.pc = self.exception_vector(use_refill);
        self.exception_taken = true;
    }

    /// Traduce `vaddr` a dirección física aplicando segmentación MIPS32 +
    /// privilegio + TLB. Devuelve `None` si ya disparó una excepción.
    ///
    /// - kuseg (0x0000_0000-0x7FFF_FFFF): siempre vía TLB, accesible en
    ///   cualquier modo.
    /// - kseg0/kseg1: mapeo directo (resta el base), sin TLB, solo kernel.
    /// - kseg2/kseg3: vía TLB, solo kernel.
    fn translate_addr(
        &mut self,
        vaddr: u32,
        is_store: bool,
        instr_pc: u32,
        in_delay_slot: bool,
    ) -> Option<u32> {
        let user = self.cop0.is_user_mode();

        match vaddr {
            0x0000_0000..=0x7FFF_FFFF => {} // kuseg: cae al lookup de TLB de abajo
            0x8000_0000..=0x9FFF_FFFF => {
                if user {
                    self.raise_addr_error(is_store, vaddr, instr_pc, in_delay_slot);
                    return None;
                }
                return Some(vaddr - 0x8000_0000); // kseg0
            }
            0xA000_0000..=0xBFFF_FFFF => {
                if user {
                    self.raise_addr_error(is_store, vaddr, instr_pc, in_delay_slot);
                    return None;
                }
                return Some(vaddr - 0xA000_0000); // kseg1
            }
            _ => {
                // kseg2/kseg3
                if user {
                    self.raise_addr_error(is_store, vaddr, instr_pc, in_delay_slot);
                    return None;
                }
            }
        }

        let asid = (self.cop0.read(CP0_REG_ENTRYHI) & 0xFF) as u8;
        let idx = match self.tlb.find(vaddr, asid) {
            Some(i) => i,
            None => {
                let exc = if is_store { ExcCode::TlbS } else { ExcCode::TlbL };
                self.raise_tlb_exception(exc, instr_pc, in_delay_slot, vaddr, true);
                return None;
            }
        };

        let e = &self.tlb.entries[idx];
        let odd = ((vaddr >> 12) & 1) != 0;
        let (pfn, dirty, valid) = if odd {
            (e.pfn1, e.d1, e.v1)
        } else {
            (e.pfn0, e.d0, e.v0)
        };

        if !valid {
            let exc = if is_store { ExcCode::TlbS } else { ExcCode::TlbL };
            self.raise_tlb_exception(exc, instr_pc, in_delay_slot, vaddr, false);
            return None;
        }
        if is_store && !dirty {
            self.raise_tlb_exception(ExcCode::Mod, instr_pc, in_delay_slot, vaddr, false);
            return None;
        }

        Some((pfn << 12) | (vaddr & 0xFFF))
    }

    fn tlb_entry_from_regs(&self) -> TlbEntry {
        let entry_hi = self.cop0.read(CP0_REG_ENTRYHI);
        let lo0 = self.cop0.read(CP0_REG_ENTRYLO0);
        let lo1 = self.cop0.read(CP0_REG_ENTRYLO1);
        TlbEntry {
            vpn2: entry_hi >> 13,
            asid: (entry_hi & 0xFF) as u8,
            global: (lo0 & 1) != 0 && (lo1 & 1) != 0,
            page_mask: self.cop0.read(CP0_REG_PAGEMASK),
            pfn0: lo0 >> 6,
            d0: (lo0 >> 2) & 1 != 0,
            v0: (lo0 >> 1) & 1 != 0,
            pfn1: lo1 >> 6,
            d1: (lo1 >> 2) & 1 != 0,
            v1: (lo1 >> 1) & 1 != 0,
        }
    }

    fn tlb_regs_from_entry(&mut self, e: &TlbEntry) {
        let g = if e.global { 1 } else { 0 };
        self.cop0
            .write(CP0_REG_ENTRYHI, (e.vpn2 << 13) | (e.asid as u32));
        self.cop0.write(
            CP0_REG_ENTRYLO0,
            (e.pfn0 << 6) | ((e.d0 as u32) << 2) | ((e.v0 as u32) << 1) | g,
        );
        self.cop0.write(
            CP0_REG_ENTRYLO1,
            (e.pfn1 << 6) | ((e.d1 as u32) << 2) | ((e.v1 as u32) << 1) | g,
        );
        self.cop0.write(CP0_REG_PAGEMASK, e.page_mask);
    }

    /// Índice "Random" entre Wired y TLB_ENTRIES-1, recalculado a partir de
    /// Count (no hay un registro Random que tiquee aparte).
    fn tlb_random_index(&self) -> usize {
        let wired = (self.cop0.read(CP0_REG_WIRED) as usize).min(TLB_ENTRIES - 1);
        let span = TLB_ENTRIES - wired;
        if span == 0 {
            0
        } else {
            wired + (self.cop0.read(CP0_REG_COUNT) as usize % span)
        }
    }

    fn mem_read8(&mut self, bus: &mut MemoryBus, vaddr: u32, instr_pc: u32, in_delay_slot: bool) -> Option<u8> {
        let paddr = self.translate_addr(vaddr, false, instr_pc, in_delay_slot)?;
        match bus.read8(paddr) {
            Ok(v) => Some(v),
            Err(_) => {
                self.raise_exception(ExcCode::DBE, instr_pc, in_delay_slot, Some(vaddr));
                None
            }
        }
    }

    fn mem_read16(&mut self, bus: &mut MemoryBus, vaddr: u32, instr_pc: u32, in_delay_slot: bool) -> Option<u16> {
        if vaddr % 2 != 0 {
            self.raise_addr_error(false, vaddr, instr_pc, in_delay_slot);
            return None;
        }
        let paddr = self.translate_addr(vaddr, false, instr_pc, in_delay_slot)?;
        match bus.read16(paddr) {
            Ok(v) => Some(v),
            Err(_) => {
                self.raise_exception(ExcCode::DBE, instr_pc, in_delay_slot, Some(vaddr));
                None
            }
        }
    }

    fn mem_read32(&mut self, bus: &mut MemoryBus, vaddr: u32, instr_pc: u32, in_delay_slot: bool) -> Option<u32> {
        if vaddr % 4 != 0 {
            self.raise_addr_error(false, vaddr, instr_pc, in_delay_slot);
            return None;
        }
        let paddr = self.translate_addr(vaddr, false, instr_pc, in_delay_slot)?;
        match bus.read32(paddr) {
            Ok(v) => Some(v),
            Err(_) => {
                self.raise_exception(ExcCode::DBE, instr_pc, in_delay_slot, Some(vaddr));
                None
            }
        }
    }

    fn mem_write8(&mut self, bus: &mut MemoryBus, vaddr: u32, val: u8, instr_pc: u32, in_delay_slot: bool) -> Option<()> {
        let paddr = self.translate_addr(vaddr, true, instr_pc, in_delay_slot)?;
        match bus.write8(paddr, val) {
            Ok(()) => Some(()),
            Err(MemoryError::RomWrite(_)) => Some(()), // escritura a ROM: no-op silencioso (como en HW real)
            Err(_) => {
                self.raise_exception(ExcCode::DBE, instr_pc, in_delay_slot, Some(vaddr));
                None
            }
        }
    }

    fn mem_write16(&mut self, bus: &mut MemoryBus, vaddr: u32, val: u16, instr_pc: u32, in_delay_slot: bool) -> Option<()> {
        if vaddr % 2 != 0 {
            self.raise_addr_error(true, vaddr, instr_pc, in_delay_slot);
            return None;
        }
        let paddr = self.translate_addr(vaddr, true, instr_pc, in_delay_slot)?;
        match bus.write16(paddr, val) {
            Ok(()) => Some(()),
            Err(MemoryError::RomWrite(_)) => Some(()),
            Err(_) => {
                self.raise_exception(ExcCode::DBE, instr_pc, in_delay_slot, Some(vaddr));
                None
            }
        }
    }

    fn mem_write32(&mut self, bus: &mut MemoryBus, vaddr: u32, val: u32, instr_pc: u32, in_delay_slot: bool) -> Option<()> {
        if vaddr % 4 != 0 {
            self.raise_addr_error(true, vaddr, instr_pc, in_delay_slot);
            return None;
        }
        let paddr = self.translate_addr(vaddr, true, instr_pc, in_delay_slot)?;
        match bus.write32(paddr, val) {
            Ok(()) => Some(()),
            Err(MemoryError::RomWrite(_)) => Some(()),
            Err(_) => {
                self.raise_exception(ExcCode::DBE, instr_pc, in_delay_slot, Some(vaddr));
                None
            }
        }
    }

    pub fn fetch(&mut self, bus: &mut MemoryBus, instr_pc: u32, in_delay_slot: bool) -> Option<u32> {
        let pc = self.registers.get_pc();
        if pc % 4 != 0 {
            self.raise_addr_error(false, pc, instr_pc, in_delay_slot);
            return None;
        }
        let paddr = self.translate_addr(pc, false, instr_pc, in_delay_slot)?;
        match bus.read32(paddr) {
            Ok(instr) => {
                self.registers.special.pc = pc.wrapping_add(4);
                Some(instr)
            }
            Err(_) => {
                self.raise_exception(ExcCode::IBE, instr_pc, in_delay_slot, Some(pc));
                None
            }
        }
    }

    pub fn decode(&self, instr: u32) -> Instruction {
        let decoded = Instruction::decode(instr);
        decoded
    }

    pub fn execute(
        &mut self,
        bus: &mut MemoryBus,
        instr: Instruction,
        instr_pc: u32,
        in_delay_slot: bool,
    ) -> u32 {
        match instr {
            Instruction::RType(r) => {
                let rs_val = self.registers.read(r.rs as usize);
                let rt_val = self.registers.read(r.rt as usize);
                let shamt = r.shamt;

                // Control flow ops (JR / JALR)
                match r.funct {
                    0x01 => {
                        // MOVCI (MOVF / MOVT)
                        // rd <- rs if (FCC0 == bit(rt,0))
                        let cond_true = (r.rt & 0x01) != 0; // si bit0 = 1 → MOVT, si = 0 → MOVF
                        let fcc0 = self.get_fcc0();

                        if fcc0 == cond_true {
                            let val = self.registers.read(r.rs as usize);
                            self.registers.write(r.rd as usize, val);
                        }
                        return 0;
                    }

                    0x08 => {
                        // JR
                        let target = rs_val;
                        if let Some(v) = self.fetch(bus, instr_pc, true) {
                            let d = self.decode(v);
                            self.execute(bus, d, instr_pc, true);
                        }
                        if !self.exception_taken {
                            self.registers.special.pc = target;
                        }
                        return 0;
                    }
                    0x09 => {
                        // JALR
                        let link = self.registers.get_pc().wrapping_add(4); // PC + 8
                        let target = rs_val;
                        if r.rd != 0 {
                            self.registers.write(r.rd as usize, link);
                        }
                        if let Some(v) = self.fetch(bus, instr_pc, true) {
                            let d = self.decode(v);
                            self.execute(bus, d, instr_pc, true);
                        }
                        if !self.exception_taken {
                            self.registers.special.pc = target;
                        }
                        return 0;
                    }
                    0x0C => {
                        // SYSCALL
                        self.raise_exception(ExcCode::Sys, instr_pc, in_delay_slot, None);
                        return 0;
                    }
                    0x0D => {
                        // BREAK
                        self.raise_exception(ExcCode::Bp, instr_pc, in_delay_slot, None);
                        return 0;
                    }
                    0x34 => {
                        // TEQ
                        if rs_val == rt_val {
                            self.raise_exception(ExcCode::Tr, instr_pc, in_delay_slot, None);
                        }
                        return 0;
                    }
                    0x0A => {
                        // MOVZ rd, rs, rt
                        if rt_val == 0 {
                            self.registers.write(r.rd as usize, rs_val);
                        }
                        return 0;
                    }
                    0x0B => {
                        // MOVN rd, rs, rt
                        if rt_val != 0 {
                            self.registers.write(r.rd as usize, rs_val);
                        }
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

                if res.overflow {
                    self.raise_exception(ExcCode::Ov, instr_pc, in_delay_slot, None);
                    return 0;
                }

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
                            let delay = self.fetch(bus, instr_pc, true);
                            if let Some(v) = delay {
                                let delay_decoded = self.decode(v);
                                self.execute(bus, delay_decoded, instr_pc, true);
                            }
                            if !self.exception_taken {
                                self.registers.special.pc = pc_next.wrapping_add(offset);
                            }
                        }
                        return 0;
                    }
                    0x05 => {
                        // BNE
                        if rs_val != rt_val {
                            let offset = ((i.imm as i16 as i32) << 2) as u32;
                            let delay = self.fetch(bus, instr_pc, true);
                            if let Some(v) = delay {
                                let delay_decoded = self.decode(v);
                                self.execute(bus, delay_decoded, instr_pc, true);
                            }
                            if !self.exception_taken {
                                self.registers.special.pc = pc_next.wrapping_add(offset);
                            }
                        }
                        return 0;
                    }
                    0x06 => {
                        // BLEZ
                        if (rs_val as i32) <= 0 {
                            let offset = ((i.imm as i16 as i32) << 2) as u32;
                            let delay = self.fetch(bus, instr_pc, true);
                            if let Some(v) = delay {
                                let delay_decoded = self.decode(v);
                                self.execute(bus, delay_decoded, instr_pc, true);
                            }
                            if !self.exception_taken {
                                self.registers.special.pc = pc_next.wrapping_add(offset);
                            }
                        }
                        return 0;
                    }
                    0x07 => {
                        // BGTZ
                        if (rs_val as i32) > 0 {
                            let offset = ((i.imm as i16 as i32) << 2) as u32;
                            let delay = self.fetch(bus, instr_pc, true);
                            if let Some(v) = delay {
                                let delay_decoded = self.decode(v);
                                self.execute(bus, delay_decoded, instr_pc, true);
                            }
                            if !self.exception_taken {
                                self.registers.special.pc = pc_next.wrapping_add(offset);
                            }
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
                        if let Some(v) = self.fetch(bus, instr_pc, true) {
                            let d = self.decode(v);
                            self.execute(bus, d, instr_pc, true);
                        }

                        if self.exception_taken {
                            return 0;
                        }

                        if cond {
                            if link {
                                self.registers.write(31, pc_next.wrapping_add(4)); // PC+8
                            }
                            let branch_target =
                                pc_next.wrapping_add(((i.imm as i16 as i32) << 2) as u32);
                            self.registers.special.pc = branch_target;
                        }
                        return 0;
                    }

                    0x23 => {
                        // LW rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        if let Some(val) = self.mem_read32(bus, addr, instr_pc, in_delay_slot) {
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
                        self.mem_write32(bus, addr, rt_val, instr_pc, in_delay_slot);
                        return 0;
                    }
                    0x20 => {
                        // LB rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        if let Some(b) = self.mem_read8(bus, addr, instr_pc, in_delay_slot) {
                            let val = b as i8 as i32 as u32;
                            if i.rt != 0 {
                                self.registers.write(i.rt as usize, val);
                            }
                            return val;
                        }
                        return 0;
                    }
                    0x24 => {
                        // LBU rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        if let Some(b) = self.mem_read8(bus, addr, instr_pc, in_delay_slot) {
                            let val = b as u32;
                            if i.rt != 0 {
                                self.registers.write(i.rt as usize, val);
                            }
                            return val;
                        }
                        return 0;
                    }
                    0x21 => {
                        // LH rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        if let Some(h) = self.mem_read16(bus, addr, instr_pc, in_delay_slot) {
                            let val = h as i16 as i32 as u32;
                            if i.rt != 0 {
                                self.registers.write(i.rt as usize, val);
                            }
                            return val;
                        }
                        return 0;
                    }
                    0x25 => {
                        // LHU rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        if let Some(h) = self.mem_read16(bus, addr, instr_pc, in_delay_slot) {
                            let val = h as u32;
                            if i.rt != 0 {
                                self.registers.write(i.rt as usize, val);
                            }
                            return val;
                        }
                        return 0;
                    }
                    0x28 => {
                        // SB rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = (rt_val & 0xFF) as u8;
                        self.mem_write8(bus, addr, val, instr_pc, in_delay_slot);
                        return 0;
                    }
                    0x29 => {
                        // SH rt, offset(rs)
                        let addr = rs_val.wrapping_add(imm_u);
                        let val = (rt_val & 0xFFFF) as u16;
                        self.mem_write16(bus, addr, val, instr_pc, in_delay_slot);
                        return 0;
                    }
                    0x08 => {
                        // ADDI ya pasa por la rama ALU normal de abajo, pero
                        // el overflow trap requiere chequear antes de
                        // escribir rt: lo manejamos junto al resto de I-type
                        // ALU ops al final de este bloque.
                    }
                    0x31 => {
                        // LWC1 (Load Word to Cop1)
                        let addr = rs_val.wrapping_add(imm_u);
                        if let Some(val) = self.mem_read32(bus, addr, instr_pc, in_delay_slot) {
                            let f = f32::from_bits(val);
                            self.cop1.write_f(i.rt as usize, f as f64);
                            return val;
                        }
                        return 0;
                    }

                    0x39 => {
                        // SWC1 (Store Word from Cop1)
                        let addr = rs_val.wrapping_add(imm_u);
                        let fval = self.cop1.read_f(i.rt as usize) as f32;
                        self.mem_write32(bus, addr, fval.to_bits(), instr_pc, in_delay_slot);
                        return 0;
                    }

                    0x35 => {
                        // LDC1 (Load Double)
                        let addr = rs_val.wrapping_add(imm_u);
                        if let Some(lo) = self.mem_read32(bus, addr, instr_pc, in_delay_slot) {
                            if let Some(hi) =
                                self.mem_read32(bus, addr + 4, instr_pc, in_delay_slot)
                            {
                                let bits = ((hi as u64) << 32) | lo as u64;
                                self.cop1.write_bits(i.rt as usize, bits);
                            }
                        }
                        return 0;
                    }

                    0x3D => {
                        // SDC1 (Store Double)
                        let addr = rs_val.wrapping_add(imm_u);
                        let bits = self.cop1.read_bits(i.rt as usize);
                        let lo = (bits & 0xFFFF_FFFF) as u32;
                        let hi = (bits >> 32) as u32;
                        if self
                            .mem_write32(bus, addr, lo, instr_pc, in_delay_slot)
                            .is_some()
                        {
                            self.mem_write32(bus, addr + 4, hi, instr_pc, in_delay_slot);
                        }
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

                if res.overflow {
                    self.raise_exception(ExcCode::Ov, instr_pc, in_delay_slot, None);
                    return 0;
                }

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

                if let Some(v) = self.fetch(bus, instr_pc, true) {
                    let delay_decoded = self.decode(v);
                    self.execute(bus, delay_decoded, instr_pc, true);
                }

                if !self.exception_taken {
                    self.registers.special.pc = target;
                }
                0
            }

            Instruction::Cop0(c) => {
                // CP0 solo es accesible en modo kernel, salvo que el SO haya
                // habilitado explícitamente CU0 para el proceso de usuario.
                if self.cop0.is_user_mode() && !self.cop0.status_bit(STATUS_CU0) {
                    self.cop0.set_cause_ce(0);
                    self.raise_exception(ExcCode::CpU, instr_pc, in_delay_slot, None);
                    return 0;
                }

                match c.rs {
                    0x00 => {
                        // MFC0 rt <- CP0[rd] (Random se recalcula al vuelo)
                        let val = if c.rd as usize == CP0_REG_RANDOM {
                            self.tlb_random_index() as u32
                        } else {
                            self.cop0.read(c.rd as usize)
                        };
                        if c.rt != 0 {
                            self.registers.write(c.rt as usize, val);
                        }
                        0
                    }
                    0x04 => {
                        // MTC0 CP0[rd] <- rt
                        let val = self.registers.read(c.rt as usize);
                        match c.rd as usize {
                            CP0_REG_CAUSE => {
                                // Cause: solo IP0/IP1 (interrupciones por software) son
                                // escribibles vía MTC0; ExcCode/BD/IP2-7 los controla el HW.
                                let preserved = self.cop0.cause() & !CAUSE_IP_SW_MASK;
                                self.cop0
                                    .write(CP0_REG_CAUSE, preserved | (val & CAUSE_IP_SW_MASK));
                            }
                            CP0_REG_COMPARE => {
                                // Escribir Compare limpia la interrupción de timer pendiente.
                                self.cop0.write(CP0_REG_COMPARE, val);
                                self.cop0.clear_cause_bit(CAUSE_IP7);
                            }
                            rd => self.cop0.write(rd, val),
                        }
                        0
                    }
                    0x10 => {
                        // CO bit set: el funct decide la operación privilegiada
                        match c.funct {
                            0x18 => {
                                // ERET: vuelve de la excepción
                                if self.cop0.status_bit(STATUS_ERL) {
                                    self.registers.special.pc = self.cop0.read(30); // ErrorEPC
                                    self.cop0.clear_status_bit(STATUS_ERL);
                                } else {
                                    self.registers.special.pc = self.cop0.epc();
                                    self.cop0.clear_status_bit(STATUS_EXL);
                                }
                                0
                            }
                            0x01 => {
                                // TLBR: lee la entrada en Index hacia EntryHi/EntryLo0/1/PageMask
                                let idx = (self.cop0.read(CP0_REG_INDEX) as usize)
                                    & (TLB_ENTRIES - 1);
                                let e = self.tlb.entries[idx];
                                self.tlb_regs_from_entry(&e);
                                0
                            }
                            0x02 => {
                                // TLBWI: escribe en la entrada indicada por Index
                                let idx = (self.cop0.read(CP0_REG_INDEX) as usize)
                                    & (TLB_ENTRIES - 1);
                                self.tlb.entries[idx] = self.tlb_entry_from_regs();
                                0
                            }
                            0x06 => {
                                // TLBWR: escribe en una entrada "aleatoria" (Wired..N-1)
                                let idx = self.tlb_random_index();
                                self.tlb.entries[idx] = self.tlb_entry_from_regs();
                                0
                            }
                            0x08 => {
                                // TLBP: busca por EntryHi.(VPN2,ASID) y deja el resultado en Index
                                let entry_hi = self.cop0.read(CP0_REG_ENTRYHI);
                                let asid = (entry_hi & 0xFF) as u8;
                                let vpn2 = entry_hi >> 13;
                                match self
                                    .tlb
                                    .entries
                                    .iter()
                                    .position(|e| e.vpn2 == vpn2 && (e.global || e.asid == asid))
                                {
                                    Some(i) => self.cop0.write(CP0_REG_INDEX, i as u32),
                                    None => self.cop0.write(CP0_REG_INDEX, 0x8000_0000),
                                }
                                0
                            }
                            0x20 => 0, // WAIT: no-op
                            _ => {
                                println!(
                                    "[COP0] funct=0x{:02X} no implementado en PC={:#010X}",
                                    c.funct, instr_pc
                                );
                                0
                            }
                        }
                    }
                    _ => {
                        println!(
                            "[COP0] rs=0x{:02X} no implementado en PC={:#010X}",
                            c.rs, instr_pc
                        );
                        0
                    }
                }
            }

            Instruction::Cop1(fp) => match fp.fmt {
                0x00 => {
                    // MFC1 rt <- fs (low 32b de FPR)
                    let rt = fp.ft as usize; // en MIPS rt va donde tú guardaste ft
                    let fs = fp.fs as usize;
                    let val = self.cop1.read_s_bits(fs);
                    self.registers.write(rt, val); // ajusta a tu API
                    0
                }
                0x04 => {
                    // MTC1 fs <- rt (low 32b a FPR)
                    let rt = fp.ft as usize;
                    let fs = fp.fs as usize;
                    let val = self.registers.read(rt);
                    self.cop1.write_s_bits(fs, val);
                    0
                }
                0x02 => {
                    // CFC1 rt <- FCSR (o FIR si usas fs para seleccionar)
                    let rt = fp.ft as usize;
                    let src = if fp.fs == 31 {
                        self.cop1.fcsr
                    } else {
                        self.cop1.fir
                    };
                    self.registers.write(rt, src);
                    0
                }
                0x06 => {
                    // CTC1 FCSR <- rt (o FIR)
                    let rt = fp.ft as usize;
                    let v = self.registers.read(rt);
                    if fp.fs == 31 {
                        self.cop1.fcsr = v;
                    } else {
                        self.cop1.fir = v;
                    }
                    0
                }

                0x08 => {
                    let tf = (fp.ft >> 0) & 0x1; // 0 = false, 1 = true (C.xxT vs C.xxF)
                    let nd = (fp.ft >> 1) & 0x1; // 1 = likely (BC1TL/BC1FL)
                    let cond = self.cop1.get_fcc0();

                    let take = if tf == 1 { cond } else { !cond };
                    if take {
                    } else if nd == 1 {
                    }
                    0
                }
                _ => {
                    println!(
                        "[COP1] Unhandled fmt=0x{:02X} at PC={:#010X}",
                        fp.fmt,
                        self.registers.get_pc()
                    );
                    0
                }
            },

            _ => 0,
        }
    }

    pub fn memory_access(&mut self, exec_result: u32) -> u32 {
        exec_result
    }

    pub fn writeback(&mut self, _mem_result: u32) {}
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::devices::ram::Ram;

    fn new_test_system() -> (CPU, MemoryBus) {
        let cpu = CPU::new();
        let mut bus = MemoryBus::new(true);
        bus.add_device(Box::new(Ram::new(0x0000_0000, 0x0010_0000)));
        bus.add_device(Box::new(Ram::new(0x1FC0_0000, 0x0001_0000)));
        (cpu, bus)
    }

    fn store_word(bus: &mut MemoryBus, addr: u32, val: u32) {
        bus.write32_virt(addr, val).unwrap();
    }

    #[test]
    fn syscall_jumps_to_vector_and_sets_epc() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.registers.special.pc = 0x8000_0000;
        // SYSCALL: opcode 0, funct 0x0C
        store_word(&mut bus, 0x8000_0000, 0x0000_000C);

        cpu.step(&mut bus);

        assert_eq!(cpu.registers.get_pc(), 0xBFC0_0180); // BEV=1 tras reset
        assert_eq!(cpu.cop0.epc(), 0x8000_0000);
        assert!(cpu.cop0.status_bit(STATUS_EXL));
        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::Sys.code());
        assert!(cpu.cop0.cause() & CAUSE_BD == 0);
    }

    #[test]
    fn eret_restores_pc_and_clears_exl() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.cop0.set_epc(0x8000_1234);
        cpu.cop0.clear_status_bit(STATUS_ERL); // set tras reset(); probamos el retorno normal (no de error)
        cpu.cop0.set_status_bit(STATUS_EXL);
        cpu.registers.special.pc = 0x8000_0000;
        // ERET: opcode 0x10, rs=0x10, funct=0x18
        store_word(&mut bus, 0x8000_0000, 0x42000018);

        cpu.step(&mut bus);

        assert_eq!(cpu.registers.get_pc(), 0x8000_1234);
        assert!(!cpu.cop0.status_bit(STATUS_EXL));
    }

    #[test]
    fn mtc0_mfc0_roundtrip() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.registers.write(8, 0xDEAD_BEEF);
        cpu.registers.special.pc = 0x8000_0000;
        // MTC0 $8, $12 (Status): opcode 0x10, rs=0x04, rt=8, rd=12
        store_word(&mut bus, 0x8000_0000, 0x40886000);
        cpu.step(&mut bus);
        assert_eq!(cpu.cop0.status(), 0xDEAD_BEEF);

        // MFC0 $9, $12 (Status): rs=0x00, rt=9, rd=12
        store_word(&mut bus, 0x8000_0004, 0x40096000);
        cpu.step(&mut bus);
        assert_eq!(cpu.registers.read(9), 0xDEAD_BEEF);
    }

    #[test]
    fn unaligned_load_raises_address_error() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.registers.write(4, 0x8000_0001); // dirección desalineada
        cpu.registers.special.pc = 0x8000_0000;
        // LW $5, 0($4): opcode 0x23, rs=4, rt=5
        store_word(&mut bus, 0x8000_0000, 0x8C850000);

        cpu.step(&mut bus);

        assert_eq!(cpu.registers.get_pc(), 0xBFC0_0180);
        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::AdEL.code());
        assert_eq!(cpu.cop0.badvaddr(), 0x8000_0001);
    }

    #[test]
    fn add_overflow_traps() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.registers.write(4, 0x7FFF_FFFF);
        cpu.registers.write(5, 1);
        cpu.registers.special.pc = 0x8000_0000;
        // ADD $6, $4, $5: opcode 0, funct 0x20
        store_word(&mut bus, 0x8000_0000, 0x00853020);

        cpu.step(&mut bus);

        assert_eq!(cpu.registers.get_pc(), 0xBFC0_0180);
        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::Ov.code());
        assert_eq!(cpu.registers.read(6), 0); // no se escribió rd
    }

    #[test]
    fn addu_does_not_trap_on_overflow() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.registers.write(4, 0x7FFF_FFFF);
        cpu.registers.write(5, 1);
        cpu.registers.special.pc = 0x8000_0000;
        // ADDU $6, $4, $5: opcode 0, funct 0x21
        store_word(&mut bus, 0x8000_0000, 0x00853021);

        cpu.step(&mut bus);

        assert_eq!(cpu.registers.get_pc(), 0x8000_0004);
        assert_eq!(cpu.registers.read(6), 0x8000_0000);
        assert!(!cpu.cop0.status_bit(STATUS_EXL));
    }

    #[test]
    fn timer_interrupt_fires_when_count_reaches_compare() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.cop0.write(CP0_REG_COMPARE, 1);
        // IE=1, IM7=1, BEV=1 (se preserva del reset), ERL/EXL=0
        cpu.cop0.set_status(STATUS_BEV | STATUS_IE | (1 << 15));
        cpu.registers.special.pc = 0x8000_0000;

        cpu.step(&mut bus);

        assert_eq!(cpu.cop0.read(CP0_REG_COUNT), 1);
        assert_eq!(cpu.registers.get_pc(), 0xBFC0_0180);
        assert_eq!(cpu.cop0.epc(), 0x8000_0000);
        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::Int.code());
        assert!(cpu.cop0.status_bit(STATUS_EXL));
    }

    #[test]
    fn no_interrupt_without_ie_or_matching_im() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.cop0.write(CP0_REG_COMPARE, 1);
        cpu.cop0.set_status(STATUS_BEV); // IE=0
        cpu.registers.special.pc = 0x8000_0000;
        store_word(&mut bus, 0x8000_0000, 0x0000_0000); // NOP

        cpu.step(&mut bus);

        // Count llegó a Compare (queda pendiente en Cause.IP7) pero como
        // IE=0 no se toma la excepción: se ejecuta la instrucción normal.
        assert_eq!(cpu.registers.get_pc(), 0x8000_0004);
        assert!(!cpu.cop0.status_bit(STATUS_EXL));
        assert_ne!(cpu.cop0.cause() & CAUSE_IP7, 0);
    }

    #[test]
    fn mtc0_compare_clears_pending_timer_interrupt() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.cop0.set_cause_bit(CAUSE_IP7);
        cpu.registers.write(4, 100);
        cpu.registers.special.pc = 0x8000_0000;
        // MTC0 $4, $11 (Compare): rs=0x04, rt=4, rd=11
        store_word(&mut bus, 0x8000_0000, 0x40845800);

        cpu.step(&mut bus);

        assert_eq!(cpu.cop0.read(CP0_REG_COMPARE), 100);
        assert_eq!(cpu.cop0.cause() & CAUSE_IP7, 0);
    }

    #[test]
    fn mtc0_cause_only_writes_software_interrupt_bits() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.registers.write(4, 0xFFFF_FFFF);
        cpu.registers.special.pc = 0x8000_0000;
        // MTC0 $4, $13 (Cause): rs=0x04, rt=4, rd=13
        store_word(&mut bus, 0x8000_0000, 0x40846800);

        cpu.step(&mut bus);

        assert_eq!(cpu.cop0.cause(), CAUSE_IP_SW_MASK);
    }

    #[test]
    fn kuseg_access_without_tlb_entry_raises_refill_miss() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.registers.write(4, 0x0000_2000); // kuseg, vpn2=1, sin entrada en la TLB
        cpu.registers.special.pc = 0x8000_0000;
        store_word(&mut bus, 0x8000_0000, 0x8C850000); // LW $5, 0($4)

        cpu.step(&mut bus);

        assert_eq!(cpu.registers.get_pc(), 0xBFC0_0200); // vector de TLB refill (BEV=1)
        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::TlbL.code());
        assert_eq!(cpu.cop0.badvaddr(), 0x0000_2000);
        assert_eq!((cpu.cop0.read(CP0_REG_CONTEXT) >> 4) & 0x7FFFF, 1); // BadVPN2
    }

    #[test]
    fn tlbwi_then_kuseg_load_succeeds() {
        let (mut cpu, mut bus) = new_test_system();
        bus.write32(0x10000, 0xCAFE_BABE).unwrap(); // física, donde vamos a mapear el VPN

        cpu.registers.special.pc = 0x8000_0000;
        cpu.registers.write(4, 0x2000); // EntryHi: vpn2=1, asid=0
        cpu.registers.write(5, 0x406); // EntryLo0: pfn=0x10, D=1, V=1
        cpu.registers.write(7, 0x2000); // vaddr a leer (kuseg, vpn2=1, página par)

        store_word(&mut bus, 0x8000_0000, 0x40845000); // MTC0 $4, $10 (EntryHi)
        store_word(&mut bus, 0x8000_0004, 0x40851000); // MTC0 $5, $2  (EntryLo0)
        store_word(&mut bus, 0x8000_0008, 0x42000002); // TLBWI
        store_word(&mut bus, 0x8000_000C, 0x8CE60000); // LW $6, 0($7)

        for _ in 0..4 {
            cpu.step(&mut bus);
        }

        assert_eq!(cpu.registers.read(6), 0xCAFE_BABE);
        assert!(!cpu.cop0.status_bit(STATUS_EXL)); // sin excepciones en el camino
    }

    #[test]
    fn tlb_hit_invalid_entry_raises_general_vector_not_refill() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.tlb.entries[0] = TlbEntry {
            vpn2: 1,
            asid: 0,
            global: false,
            page_mask: 0,
            pfn0: 0x10,
            d0: false,
            v0: false, // inválida
            pfn1: 0,
            d1: false,
            v1: false,
        };
        cpu.registers.write(4, 0x2000);
        cpu.registers.special.pc = 0x8000_0000;
        store_word(&mut bus, 0x8000_0000, 0x8C850000); // LW $5, 0($4)

        cpu.step(&mut bus);

        assert_eq!(cpu.registers.get_pc(), 0xBFC0_0180); // vector general, no refill
        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::TlbL.code());
    }

    #[test]
    fn store_to_readonly_page_raises_mod() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.tlb.entries[0] = TlbEntry {
            vpn2: 1,
            asid: 0,
            global: false,
            page_mask: 0,
            pfn0: 0x10,
            d0: false, // sin permiso de escritura
            v0: true,
            pfn1: 0,
            d1: false,
            v1: false,
        };
        cpu.registers.write(4, 0x2000);
        cpu.registers.write(5, 0x1234);
        cpu.registers.special.pc = 0x8000_0000;
        store_word(&mut bus, 0x8000_0000, 0xAC850000); // SW $5, 0($4)

        cpu.step(&mut bus);

        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::Mod.code());
        assert_eq!(cpu.registers.get_pc(), 0xBFC0_0180); // Mod siempre va por el vector general
    }

    #[test]
    fn tlb_entry_with_different_asid_does_not_match() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.tlb.entries[0] = TlbEntry {
            vpn2: 1,
            asid: 7, // EntryHi.ASID por defecto es 0: no matchea
            global: false,
            page_mask: 0,
            pfn0: 0x10,
            d0: true,
            v0: true,
            pfn1: 0,
            d1: false,
            v1: false,
        };
        cpu.registers.write(4, 0x2000);
        cpu.registers.special.pc = 0x8000_0000;
        store_word(&mut bus, 0x8000_0000, 0x8C850000); // LW $5, 0($4)

        cpu.step(&mut bus);

        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::TlbL.code());
        assert_eq!(cpu.registers.get_pc(), 0xBFC0_0200); // miss real -> refill
    }

    #[test]
    fn user_mode_accessing_kseg0_raises_address_error() {
        let (mut cpu, mut bus) = new_test_system();
        // Mapeo identidad para que el propio código de usuario en kuseg sea fetcheable.
        cpu.tlb.entries[0] = TlbEntry {
            vpn2: 0,
            asid: 0,
            global: true,
            page_mask: 0,
            pfn0: 0,
            d0: true,
            v0: true,
            pfn1: 0,
            d1: true,
            v1: true,
        };
        cpu.cop0.set_status(STATUS_KSU_USER);
        cpu.registers.write(4, 0x8000_0000); // kseg0: ilegal desde modo usuario
        cpu.registers.special.pc = 0x0000_0000;
        store_word(&mut bus, 0x0000_0000, 0x8C850000); // LW $5, 0($4)

        cpu.step(&mut bus);

        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::AdEL.code());
        assert_eq!(cpu.cop0.badvaddr(), 0x8000_0000);
    }

    #[test]
    fn user_mode_cop0_access_raises_coprocessor_unusable() {
        let (mut cpu, mut bus) = new_test_system();
        cpu.tlb.entries[0] = TlbEntry {
            vpn2: 0,
            asid: 0,
            global: true,
            page_mask: 0,
            pfn0: 0,
            d0: true,
            v0: true,
            pfn1: 0,
            d1: true,
            v1: true,
        };
        cpu.cop0.set_status(STATUS_KSU_USER);
        cpu.registers.special.pc = 0x0000_0000;
        store_word(&mut bus, 0x0000_0000, 0x40896000); // MTC0 $9, $12 (Status), ilegal en user mode

        cpu.step(&mut bus);

        assert_eq!((cpu.cop0.cause() >> 2) & 0x1F, ExcCode::CpU.code());
        assert_eq!((cpu.cop0.cause() >> 28) & 0x3, 0); // CE=0: coprocesador 0
    }
}
