//===-- Z80InstrInfo.cpp - Z80 Instruction Information --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the Z80 implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "Z80InstrInfo.h"
#include "Z80.h"
#include "Z80Subtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
using namespace llvm;

#define DEBUG_TYPE "z80-instr-info"

#define GET_INSTRINFO_CTOR_DTOR
#include "Z80GenInstrInfo.inc"

// Pin the vtable to this file.
void Z80InstrInfo::anchor() {}

Z80InstrInfo::Z80InstrInfo(Z80Subtarget &STI)
    : Z80GenInstrInfo((STI.is24Bit() ? Z80::ADJCALLSTACKDOWN24
                                     : Z80::ADJCALLSTACKDOWN16),
                      (STI.is24Bit() ? Z80::ADJCALLSTACKUP24
                                     : Z80::ADJCALLSTACKUP16)),
      Subtarget(STI), RI(STI.getTargetTriple()) {
}

/// Return the inverse of the specified condition,
/// e.g. turning COND_E to COND_NE.
Z80::CondCode Z80::GetOppositeBranchCondition(Z80::CondCode CC) {
  return Z80::CondCode(CC ^ 1);
}

bool Z80InstrInfo::isUnpredicatedTerminator(const MachineInstr &MI) const {
  if (!MI.isTerminator()) return false;

  // Conditional branch is a special case.
  if (MI.isBranch() && !MI.isBarrier())
    return true;
  if (!MI.isPredicable())
    return true;
  return !isPredicated(MI);
}

bool Z80InstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                 MachineBasicBlock *&TBB,
                                 MachineBasicBlock *&FBB,
                                 SmallVectorImpl<MachineOperand> &Cond,
                                 bool AllowModify) const {
  // Start from the bottom of the block and work up, examining the
  // terminator instructions.
  MachineBasicBlock::iterator I = MBB.end(), UnCondBrIter = I;
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugValue())
      continue;

    // Working from the bottom, when we see a non-terminator instruction, we're
    // done.
    if (!isUnpredicatedTerminator(*I))
      break;

    // A terminator that isn't a branch can't easily be handled by this
    // analysis.
    if (!I->isBranch())
      return true;

    // Cannot handle indirect branches.
    if (I->getOpcode() == Z80::JPr)
      return true;

    // Handle unconditional branches.
    if (I->getOpcode() == Z80::JQ) {
      UnCondBrIter = I;

      if (!AllowModify) {
        TBB = I->getOperand(0).getMBB();
        continue;
      }

      // If the block has any instructions after a JMP, delete them.
      while (std::next(I) != MBB.end())
        std::next(I)->eraseFromParent();
      Cond.clear();
      FBB = nullptr;

      // Delete the JMP if it's equivalent to a fall-through.
      if (MBB.isLayoutSuccessor(I->getOperand(0).getMBB())) {
        TBB = nullptr;
        I->eraseFromParent();
        I = MBB.end();
        UnCondBrIter = I;
        continue;
      }

      // TBB is used to indicate the unconditional destination.
      TBB = I->getOperand(0).getMBB();
      continue;
    }

    // Handle conditional branches.
    assert(I->getOpcode() == Z80::JQCC && "Invalid conditional branch");
    Z80::CondCode BranchCode = Z80::CondCode(I->getOperand(1).getImm());

    // Working from the bottom, handle the first conditional branch.
    if (Cond.empty()) {
      MachineBasicBlock *TargetBB = I->getOperand(0).getMBB();
      if (AllowModify && UnCondBrIter != MBB.end() &&
          MBB.isLayoutSuccessor(TargetBB)) {
        // If we can modify the code and it ends in something like:
        //
        //     jCC L1
        //     jmp L2
        //   L1:
        //     ...
        //   L2:
        //
        // Then we can change this to:
        //
        //     jnCC L2
        //   L1:
        //     ...
        //   L2:
        //
        // Which is a bit more efficient.
        // We conditionally jump to the fall-through block.
        BranchCode = GetOppositeBranchCondition(BranchCode);
        MachineBasicBlock::iterator OldInst = I;

        BuildMI(MBB, UnCondBrIter, MBB.findDebugLoc(I), get(Z80::JQCC))
          .addMBB(UnCondBrIter->getOperand(0).getMBB()).addImm(BranchCode);
        BuildMI(MBB, UnCondBrIter, MBB.findDebugLoc(I), get(Z80::JQ))
          .addMBB(TargetBB);

        OldInst->eraseFromParent();
        UnCondBrIter->eraseFromParent();

        // Restart the analysis.
        UnCondBrIter = MBB.end();
        I = MBB.end();
        continue;
      }

      FBB = TBB;
      TBB = I->getOperand(0).getMBB();
      Cond.push_back(MachineOperand::CreateImm(BranchCode));
      continue;
    }

    return true;
  }

  return false;
}

unsigned Z80InstrInfo::removeBranch(MachineBasicBlock &MBB,
                                    int *BytesRemoved) const {
  assert(!BytesRemoved && "code size not handled");
  MachineBasicBlock::iterator I = MBB.end();
  unsigned Count = 0;

  while (I != MBB.begin()) {
    --I;
    if (I->isDebugValue())
      continue;
    if (I->getOpcode() != Z80::JQ &&
        I->getOpcode() != Z80::JQCC &&
        I->getOpcode() != Z80::JPr)
      break;
    // Remove the branch.
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }

  return Count;
}

unsigned Z80InstrInfo::insertBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *TBB,
                                    MachineBasicBlock *FBB,
                                    ArrayRef<MachineOperand> Cond,
                                    const DebugLoc &DL,
                                    int *BytesAdded) const {
  // Shouldn't be a fall through.
  assert(TBB && "InsertBranch must not be told to insert a fallthrough");
  assert(Cond.size() <= 1 && "Z80 branch conditions have one component!");
  assert(!BytesAdded && "code size not handled");

  if (Cond.empty()) {
    // Unconditional branch?
    assert(!FBB && "Unconditional branch with multiple successors!");
    BuildMI(&MBB, DL, get(Z80::JQ)).addMBB(TBB);
    return 1;
  }

  // Conditional branch.
  unsigned Count = 0;
  BuildMI(&MBB, DL, get(Z80::JQCC)).addMBB(TBB).addImm(Cond[0].getImm());
  ++Count;

  // If FBB is null, it is implied to be a fall-through block.
  if (!FBB) {
    // Two-way Conditional branch. Insert the second branch.
    BuildMI(&MBB, DL, get(Z80::JQ)).addMBB(FBB);
    ++Count;
  }
  return Count;
}

bool Z80::splitReg(
    unsigned ByteSize, unsigned Opc8, unsigned Opc16, unsigned Opc24,
    unsigned &RC, unsigned &LoOpc, unsigned &LoIdx, unsigned &HiOpc,
    unsigned &HiIdx, unsigned &HiOff, bool Has16BitEZ80Ops) {
  switch (ByteSize) {
  default: llvm_unreachable("Unexpected Size!");
  case 1:
    RC = Z80::R8RegClassID;
    LoOpc = HiOpc = Opc8;
    LoIdx = HiIdx = Z80::NoSubRegister;
    HiOff = 0;
    return false;
  case 2:
    if (Has16BitEZ80Ops) {
      RC = Z80::R16RegClassID;
      LoOpc = HiOpc = Opc16;
      LoIdx = HiIdx = Z80::NoSubRegister;
      HiOff = 0;
      return false;
    }
    RC = Z80::R16RegClassID;
    LoOpc = HiOpc = Opc8;
    LoIdx = Z80::sub_low;
    HiIdx = Z80::sub_high;
    HiOff = 1;
    return true;
  case 3:
    // Legalization should have taken care of this if we don't have eZ80 ops
    //assert(Is24Bit && HasEZ80Ops && "Need eZ80 24-bit load/store");
    RC = Z80::R24RegClassID;
    LoOpc = HiOpc = Opc24;
    LoIdx = HiIdx = Z80::NoSubRegister;
    HiOff = 0;
    return false;
//case 4:
//  // Legalization should have taken care of this if we don't have eZ80 ops
//  assert(HasEZ80Ops() && "Need eZ80 word load/store");
//  RC = Z80::R32RegClassID;
//  LoOpc = Opc24;
//  HiOpc = Opc8;
//  LoIdx = Z80::sub_long;
//  HiIdx = Z80::sub_top;
//  HiOff = 3;
//  return true;
  }
}

bool Z80InstrInfo::canExchange(unsigned RegA, unsigned RegB) const {
  // The only regs that can be directly exchanged are DE and HL, in any order.
  bool DE = false, HL = false;
  for (unsigned Reg : {RegA, RegB}) {
    if (RI.isSubRegisterEq(Z80::UDE, Reg))
      DE = true;
    else if (RI.isSubRegisterEq(Z80::UHL, Reg))
      HL = true;
  }
  return DE && HL;
}

void Z80InstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator MI,
                               const DebugLoc &DL, unsigned DstReg,
                               unsigned SrcReg, bool KillSrc) const {
  DEBUG(dbgs() << RI.getName(DstReg) << " = " << RI.getName(SrcReg) << '\n');
  /*for (auto Regs : {std::make_pair(DstReg, &SrcReg),
                    std::make_pair(SrcReg, &DstReg)}) {
    if (Z80::R8RegClass.contains(Regs.first) &&
        (Z80::R16RegClass.contains(*Regs.second) ||
         Z80::R24RegClass.contains(*Regs.second) ||
         Z80::R32RegClass.contains(*Regs.second)))
      *Regs.second = RI.getSubReg(*Regs.second, Z80::sub_low);
    else if (Z80::R24RegClass.contains(Regs.first) &&
             Z80::R32RegClass.contains(*Regs.second))
      *Regs.second = RI.getSubReg(*Regs.second, Z80::sub_long);
  }*/
  // Identity copy.
  if (DstReg == SrcReg)
    return;
  if (Z80::R8RegClass.contains(DstReg, SrcReg)) {
    // Byte copy.
    if (Z80::G8RegClass.contains(DstReg, SrcReg)) {
      // Neither are index registers.
      BuildMI(MBB, MI, DL, get(Z80::LD8rr), DstReg)
        .addReg(SrcReg, getKillRegState(KillSrc));
    } else if (Z80::I8RegClass.contains(DstReg, SrcReg)) {
      assert(Subtarget.hasIndexHalfRegs() && "Need  index half registers");
      // Both are index registers.
      if (Z80::X8RegClass.contains(DstReg, SrcReg)) {
        BuildMI(MBB, MI, DL, get(Z80::LD8xx), DstReg)
          .addReg(SrcReg, getKillRegState(KillSrc));
      } else if (Z80::Y8RegClass.contains(DstReg, SrcReg)) {
        BuildMI(MBB, MI, DL, get(Z80::LD8yy), DstReg)
          .addReg(SrcReg, getKillRegState(KillSrc));
      } else {
        // We are copying between different index registers, so we need to use
        // an intermediate register.
        BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::PUSH24r
                                                     : Z80::PUSH16r))
          .addReg(Z80::AF);
        BuildMI(MBB, MI, DL, get(Z80::X8RegClass.contains(SrcReg) ? Z80::LD8xx
                                                                  : Z80::LD8yy),
                Z80::A).addReg(SrcReg, getKillRegState(KillSrc));
        BuildMI(MBB, MI, DL, get(Z80::X8RegClass.contains(DstReg) ? Z80::LD8xx
                                                                  : Z80::LD8yy),
                DstReg).addReg(Z80::A);
        BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::POP24r
                                                     : Z80::POP16r), Z80::AF);
      }
    } else {
      assert(Subtarget.hasIndexHalfRegs() && "Need  index half registers");
      // Only one is an index register, which isn't directly possible if one of
      // them is from HL.  If so, surround with EX DE,HL and use DE instead.
      bool NeedEX = false;
      for (unsigned *Reg : {&DstReg, &SrcReg}) {
        switch (*Reg) {
        case Z80::H: *Reg = Z80::D; NeedEX = true; break;
        case Z80::L: *Reg = Z80::E; NeedEX = true; break;
        }
      }
      unsigned ExOpc = Subtarget.is24Bit() ? Z80::EX24DE : Z80::EX16DE;
      if (NeedEX) {
        // If the prev instr was an EX DE,HL, just kill it.
        if (MI != MBB.begin() && std::prev(MI)->getOpcode() == ExOpc)
          std::prev(MI)->eraseFromParent();
        else
          BuildMI(MBB, MI, DL, get(ExOpc));
      }
      BuildMI(MBB, MI, DL,
              get(Z80::X8RegClass.contains(DstReg, SrcReg) ? Z80::LD8xx
                                                           : Z80::LD8yy),
              DstReg).addReg(SrcReg, getKillRegState(KillSrc));
      if (NeedEX)
        BuildMI(MBB, MI, DL, get(ExOpc));
    }
    return;
  }
  // Specialized word copy.
  // Special case DE/HL = HL/DE<kill> as EX DE,HL.
  bool Is24Bit = Z80::R24RegClass.contains(DstReg, SrcReg);
  if (KillSrc && Is24Bit == Subtarget.is24Bit() &&
      canExchange(DstReg, SrcReg)) {
    MachineInstrBuilder MIB = BuildMI(MBB, MI, DL,
                                      get(Is24Bit ? Z80::EX24DE : Z80::EX16DE));
    MIB->findRegisterUseOperand(SrcReg)->setIsKill();
    MIB->findRegisterDefOperand(SrcReg)->setIsDead();
    MIB->findRegisterUseOperand(DstReg)->setIsUndef();
    return;
  }
  // Special case copies from index registers when we have eZ80 ops.
  bool IsSrcIndexReg = Z80::I16RegClass.contains(SrcReg) || Z80::I24RegClass.contains(SrcReg);
  if (Subtarget.hasEZ80Ops() && IsSrcIndexReg) {
    BuildMI(MBB, MI, DL, get(Is24Bit ? Z80::LEA24ro : Z80::LEA16ro), DstReg)
      .addReg(SrcReg, getKillRegState(KillSrc)).addImm(0);
    return;
  }
  // Copies to SP.
  if (DstReg == Z80::SPS || DstReg == Z80::SPL) {
    assert((Z80::A16RegClass.contains(SrcReg) || SrcReg == Z80::DE ||
            Z80::A24RegClass.contains(SrcReg) || SrcReg == Z80::UDE) &&
           "Unimplemented");
    if (SrcReg == Z80::DE || SrcReg == Z80::UDE)
      BuildMI(MBB, MI, DL, get(Is24Bit ? Z80::EX24DE : Z80::EX16DE))
        .addReg(DstReg, RegState::ImplicitDefine)
        .addReg(SrcReg, RegState::ImplicitKill);
    BuildMI(MBB, MI, DL, get(DstReg == Z80::SPL ? Z80::LD24SP : Z80::LD16SP))
      .addReg(SrcReg, getKillRegState(KillSrc));
    if (SrcReg == Z80::DE || SrcReg == Z80::UDE)
      BuildMI(MBB, MI, DL, get(Is24Bit ? Z80::EX24DE : Z80::EX16DE))
        .addReg(DstReg, RegState::ImplicitDefine)
        .addReg(SrcReg, RegState::ImplicitKill);
    return;
  }
  // Copies from SP.
  if (SrcReg == Z80::SPS || SrcReg == Z80::SPL) {
    assert((Z80::A16RegClass.contains(DstReg) || DstReg == Z80::DE ||
            Z80::A24RegClass.contains(DstReg) || DstReg == Z80::UDE) &&
           "Unimplemented");
    if (DstReg == Z80::DE || DstReg == Z80::UDE)
      BuildMI(MBB, MI, DL, get(Is24Bit ? Z80::EX24DE : Z80::EX16DE))
        .addReg(DstReg, RegState::ImplicitDefine)
        .addReg(SrcReg, RegState::ImplicitKill);
    BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::LD24ri : Z80::LD16ri),
            DstReg).addImm(0);
    BuildMI(MBB, MI, DL, get(SrcReg == Z80::SPL ? Z80::ADD24SP : Z80::ADD16SP),
            DstReg).addReg(DstReg);
    if (DstReg == Z80::DE || DstReg == Z80::UDE)
      BuildMI(MBB, MI, DL, get(Is24Bit ? Z80::EX24DE : Z80::EX16DE))
        .addReg(DstReg, RegState::ImplicitDefine)
        .addReg(SrcReg, RegState::ImplicitKill);
    return;
  }
  // If both are 24-bit then the upper byte needs to be preserved.
  // Otherwise copies of index registers may need to use this method if:
  // - We are optimizing for size and exactly one reg is an index reg because
  //     PUSH SrcReg \ POP DstReg is (2 + NumIndexRegs) bytes but slower
  //     LD DstRegLo,SrcRegLo \ LD DstRegHi,SrcRegHi is 4 bytes but faster
  // - We don't have undocumented half index copies
  bool IsDstIndexReg = Z80::I16RegClass.contains(DstReg) || Z80::I24RegClass.contains(DstReg);
  unsigned NumIndexRegs = IsSrcIndexReg + IsDstIndexReg;
  bool OptSize = MBB.getParent()->getFunction()->getAttributes()
    .hasAttribute(AttributeSet::FunctionIndex, Attribute::OptimizeForSize);
  if (Is24Bit || (NumIndexRegs == 1 && OptSize) ||
      (NumIndexRegs && !Subtarget.hasIndexHalfRegs())) {
    BuildMI(MBB, MI, DL, get(Is24Bit ? Z80::PUSH24r : Z80::PUSH16r))
      .addReg(SrcReg, getKillRegState(KillSrc));
    BuildMI(MBB, MI, DL, get(Is24Bit ? Z80::POP24r : Z80::POP16r), DstReg);
    return;
  }
  // Otherwise, implement as two copies. A 16-bit copy should copy high and low
  // 8 bits separately. A 32-bit copy should copy high 8 bits and low 24 bits.
  bool Is32Bit = Z80::R32RegClass.contains(DstReg, SrcReg);
  assert((Is32Bit || Z80::R16RegClass.contains(DstReg, SrcReg)) &&
         "Unknown copy width");
  unsigned SubLo = Is32Bit ? Z80::sub_long : Z80::sub_low;
  unsigned SubHi = Is32Bit ? Z80::sub_top : Z80::sub_high;
  unsigned DstLoReg = RI.getSubReg(DstReg, SubLo);
  unsigned SrcLoReg = RI.getSubReg(SrcReg, SubLo);
  unsigned DstHiReg = RI.getSubReg(DstReg, SubHi);
  unsigned SrcHiReg = RI.getSubReg(SrcReg, SubHi);
  bool DstLoSrcHiOverlap = RI.regsOverlap(DstLoReg, SrcHiReg);
  bool SrcLoDstHiOverlap = RI.regsOverlap(SrcLoReg, DstHiReg);
  if (DstLoSrcHiOverlap && SrcLoDstHiOverlap) {
    assert(KillSrc &&
           "Both parts of SrcReg and DstReg overlap but not killing source!");
    // e.g. EUHL = LUDE so just swap the operands
    unsigned OtherReg;
    if (canExchange(DstLoReg, SrcLoReg)) {
      BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::EX24DE : Z80::EX16DE))
        .addReg(DstReg, RegState::ImplicitDefine)
        .addReg(SrcReg, RegState::ImplicitKill);
    } else if ((OtherReg = DstLoReg, RI.isSubRegisterEq(Z80::UHL, SrcLoReg)) ||
               (OtherReg = SrcLoReg, RI.isSubRegisterEq(Z80::UHL, DstLoReg))) {
      BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::PUSH24r : Z80::PUSH16r))
        .addReg(OtherReg, RegState::Kill);
      BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::EX24SP : Z80::EX16SP));
      BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::POP24r : Z80::POP16r),
              OtherReg);
    } else {
      BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::PUSH24r : Z80::PUSH16r))
        .addReg(SrcLoReg, RegState::Kill);
      BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::PUSH24r : Z80::PUSH16r))
        .addReg(DstLoReg, RegState::Kill);
      BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::POP24r : Z80::POP16r),
              SrcLoReg);
      BuildMI(MBB, MI, DL, get(Subtarget.is24Bit() ? Z80::POP24r : Z80::POP16r),
              DstLoReg);
    }
    // Check if top needs to be moved (e.g. EUHL = HUDE).
    unsigned DstHiIdx = RI.getSubRegIndex(SrcLoReg, DstHiReg);
    unsigned SrcHiIdx = RI.getSubRegIndex(DstLoReg, SrcHiReg);
    if (DstHiIdx != SrcHiIdx)
      copyPhysReg(MBB, MI, DL, DstHiReg,
                  RI.getSubReg(DstLoReg, SrcHiIdx), KillSrc);
  } else if (DstLoSrcHiOverlap) {
    // Copy out SrcHi before SrcLo overwrites it.
    copyPhysReg(MBB, MI, DL, DstHiReg, SrcHiReg, KillSrc);
    copyPhysReg(MBB, MI, DL, DstLoReg, SrcLoReg, KillSrc);
  } else {
    // If SrcLoDstHiOverlap then copy out SrcLo before SrcHi overwrites it,
    // otherwise the order doesn't matter.
    copyPhysReg(MBB, MI, DL, DstLoReg, SrcLoReg, KillSrc);
    copyPhysReg(MBB, MI, DL, DstHiReg, SrcHiReg, KillSrc);
  }
  --MI;
  MI->addRegisterDefined(DstReg, &RI);
  if (KillSrc)
    MI->addRegisterKilled(SrcReg, &RI, true);
}

static const MachineInstrBuilder &
addSubReg(const MachineInstrBuilder &MIB, unsigned Reg, unsigned Idx,
          const MCRegisterInfo *TRI, unsigned Flags = 0) {
  if (Idx && TargetRegisterInfo::isPhysicalRegister(Reg)) {
    Reg = TRI->getSubReg(Reg, Idx);
    Idx = 0;
  }
  return MIB.addReg(Reg, Flags, Idx);
}

void Z80InstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MI,
                                       unsigned SrcReg, bool IsKill, int FI,
                                       const TargetRegisterClass *TRC,
                                       const TargetRegisterInfo *TRI) const {
  unsigned Opc;
  switch (TRC->getSize()) {
  default:
    llvm_unreachable("Unexpected regclass size");
  case 1:
    Opc = Z80::LD8or;
    break;
  case 2:
    Opc = Subtarget.hasEZ80Ops() ? Z80::LD16or : Z80::LD88or;
    break;
  case 3:
    assert(Subtarget.is24Bit() && "Only 24-bit should have 3 byte stack slots");
    Opc = Z80::LD24or;
    break;
  }
  BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(Opc)).addFrameIndex(FI).addImm(0)
    .addReg(SrcReg, getKillRegState(IsKill));
  return;
  unsigned RC, LoOpc, LoIdx, HiOpc, HiIdx, HiOff;
  bool Split =
    Z80::splitReg(TRC->getSize(), Z80::LD8or, Z80::LD16or, Z80::LD24or,
                  RC, LoOpc, LoIdx, HiOpc, HiIdx, HiOff,
                  Subtarget.has16BitEZ80Ops());
  MachineInstrBuilder LoMIB =
  addSubReg(BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(LoOpc))
            .addFrameIndex(FI).addImm(0), SrcReg, LoIdx, TRI,
            getKillRegState(IsKill));
  if (Split) {
    MachineInstrBuilder HiMIB = addSubReg(
        BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(HiOpc))
        .addFrameIndex(FI).addImm(HiOff), SrcReg, HiIdx, TRI,
        getKillRegState(IsKill));
    if (IsKill)
      HiMIB->addRegisterKilled(SrcReg, TRI, true);
    //HiMIB->bundleWithPred();
    //finalizeBundle(MBB, MachineBasicBlock::instr_iterator(LoMIB),
    //               std::next(MachineBasicBlock::instr_iterator(HiMIB)));
  }
}

void Z80InstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator MI,
                                        unsigned DstReg, int FI,
                                        const TargetRegisterClass *TRC,
                                        const TargetRegisterInfo *TRI) const {
  unsigned Opc;
  switch (TRC->getSize()) {
  default:
    llvm_unreachable("Unexpected regclass size");
  case 1:
    Opc = Z80::LD8ro;
    break;
  case 2:
    Opc = Subtarget.hasEZ80Ops() ? Z80::LD16ro : Z80::LD88ro;
    break;
  case 3:
    assert(Subtarget.is24Bit() && "Only 24-bit should have 3 byte stack slots");
    Opc = Z80::LD24ro;
    break;
  }
  BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(Opc), DstReg).addFrameIndex(FI)
    .addImm(0);
  return;
  unsigned RC, LoOpc, LoIdx, HiOpc, HiIdx, HiOff;
  bool Split =
    Z80::splitReg(TRC->getSize(), Z80::LD8ro, Z80::LD16ro, Z80::LD24ro,
                  RC, LoOpc, LoIdx, HiOpc, HiIdx, HiOff,
                  Subtarget.hasEZ80Ops());
  MachineInstrBuilder LoMIB =
  addSubReg(BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(LoOpc)), DstReg, LoIdx,
            TRI, RegState::DefineNoRead).addFrameIndex(FI).addImm(0);
  if (Split) {
    MachineInstrBuilder HiMIB = addSubReg(
        BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(HiOpc)), DstReg, HiIdx,
        TRI, RegState::Define).addFrameIndex(FI).addImm(HiOff);
    HiMIB->addRegisterDefined(DstReg, TRI);
    //HiMIB->bundleWithPred();
    //finalizeBundle(MBB, MachineBasicBlock::instr_iterator(LoMIB),
    //               std::next(MachineBasicBlock::instr_iterator(HiMIB)));
  }
}

/// Return true and the FrameIndex if the specified
/// operand and follow operands form a reference to the stack frame.
bool Z80InstrInfo::isFrameOperand(const MachineInstr &MI, unsigned int Op,
                                  int &FrameIndex) const {
  if (MI.getOperand(Op).isFI() &&
      MI.getOperand(Op + 1).isImm() && MI.getOperand(Op + 1).getImm() == 0) {
    FrameIndex = MI.getOperand(Op).getIndex();
    return true;
  }
  return false;
}

static bool isFrameLoadOpcode(int Opcode) {
  switch (Opcode) {
  default:
    return false;
  case Z80::LD8ro:
  case Z80::LD16ro:
  case Z80::LD88ro:
  case Z80::LD24ro:
    return true;
  }
}
unsigned Z80InstrInfo::isLoadFromStackSlot(const MachineInstr &MI,
                                           int &FrameIndex) const {
  return 0;
  if (isFrameLoadOpcode(MI.getOpcode()))
    if (MI.getOperand(0).getSubReg() == 0 && isFrameOperand(MI, 1, FrameIndex))
      return MI.getOperand(0).getReg();
  return 0;
}

static bool isFrameStoreOpcode(int Opcode) {
  switch (Opcode) {
  default:
    return false;
  case Z80::LD8or:
  case Z80::LD16or:
  case Z80::LD88or:
  case Z80::LD24or:
    return true;
  }
}
unsigned Z80InstrInfo::isStoreToStackSlot(const MachineInstr &MI,
                                          int &FrameIndex) const {
  return 0;
  if (isFrameStoreOpcode(MI.getOpcode()))
    if (MI.getOperand(2).getSubReg() == 0 && isFrameOperand(MI, 0, FrameIndex))
      return MI.getOperand(2).getReg();
  return 0;
}

bool Z80InstrInfo::expandPostRAPseudo(MachineInstr &MI) const {
  MachineBasicBlock &MBB = *MI.getParent();
  MachineInstrBuilder MIB(*MBB.getParent(), MI);
  const TargetRegisterInfo *TRI = &getRegisterInfo();
  DEBUG(dbgs() << "\nZ80InstrInfo::expandPostRAPseudo:"; MI.dump());
  switch (MI.getOpcode()) {
  default:
    return false;
  case Z80::RCF:
    MI.setDesc(get(Z80::OR8ar));
    MIB.addReg(Z80::A, RegState::Undef);
    break;
  case Z80::LD88rp:
  case Z80::LD88pr:
  case Z80::Cp16:
  case Z80::Cp24:
  case Z80::Cp016:
  case Z80::Cp024:
    llvm_unreachable("Unimplemented");
  case Z80::LD88ro: {
    unsigned Reg = MI.getOperand(0).getReg();
    MI.setDesc(get(Z80::LD8ro));
    MI.getOperand(0).ChangeToRegister(TRI->getSubReg(Reg, Z80::sub_low), true);
    MIB = BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(Z80::LD8ro),
                  TRI->getSubReg(Reg, Z80::sub_high))
      .addReg(MI.getOperand(1).getReg()).addImm(MI.getOperand(2).getImm() + 1);
    DEBUG(MI.dump());
    break;
  }
  case Z80::LD88or: {
    unsigned Reg = MI.getOperand(2).getReg();
    MI.setDesc(get(Z80::LD8or));
    MI.getOperand(2).ChangeToRegister(TRI->getSubReg(Reg, Z80::sub_low), false);
    MIB = BuildMI(MBB, MI, MBB.findDebugLoc(MI), get(Z80::LD8or))
      .addReg(MI.getOperand(0).getReg()).addImm(MI.getOperand(1).getImm() + 1)
      .addReg(TRI->getSubReg(Reg, Z80::sub_high));
    DEBUG(MI.dump());
    break;
  }
  case Z80::CALL16r:
  case Z80::CALL24r: {
    const char *Symbol;
    switch (MIB->getOperand(0).getReg()) {
    default: llvm_unreachable("Unexpected indcall register");
    case Z80::HL: case Z80::UHL: Symbol = "_indcallhl"; break;
    case Z80::IX: case Z80::UIX: Symbol = "_indcallix"; break;
    case Z80::IY: case Z80::UIY: Symbol = "_indcall"; break;
    }
    MI.setDesc(get(MI.getOpcode() == Z80::CALL24r ? Z80::CALL24i
                                                  : Z80::CALL16i));
    MI.getOperand(0).ChangeToES(Symbol);
    break;
  }
  case Z80::TCRETURN16i:
  case Z80::TCRETURN24i:
    MI.setDesc(get(Z80::JQ));
    break;
  case Z80::TCRETURN16r:
  case Z80::TCRETURN24r:
    MI.setDesc(get(Z80::JPr));
    break;
  }
  DEBUG(MIB->dump());
  return true;
}

bool Z80InstrInfo::analyzeCompare(const MachineInstr &MI,
                                  unsigned &SrcReg, unsigned &SrcReg2,
                                  int &CmpMask, int &CmpValue) const {
  switch (MI.getOpcode()) {
  default: return false;
  case Z80::CP8ai:
  case Z80::SUB8ai:
    SrcReg = Z80::A;
    SrcReg2 = 0;
    CmpMask = ~0;
    CmpValue = MI.getOperand(0).getImm();
    break;
  case Z80::CP8ar:
  case Z80::SUB8ar:
    SrcReg = Z80::A;
    SrcReg2 = MI.getOperand(0).getReg();
    CmpMask = ~0;
    CmpValue = 0;
    break;
  case Z80::CP8am:
  case Z80::CP8ao:
  case Z80::SUB8am:
  case Z80::SUB8ao:
    SrcReg = Z80::A;
    SrcReg2 = 0;
    CmpMask = ~0;
    CmpValue = 0;
    break;
  }
  MachineBasicBlock::const_reverse_iterator I = MI, E = MI.getParent()->rend();
  while (++I != E && I->isFullCopy())
    for (unsigned *Reg : {&SrcReg, &SrcReg2})
      if (*Reg == I->getOperand(0).getReg())
        *Reg = I->getOperand(1).getReg();
  return true;
}

bool Z80InstrInfo::optimizeCompareInstr(MachineInstr &CmpInstr,
                                        unsigned SrcReg, unsigned SrcReg2,
                                        int CmpMask, int CmpValue,
                                        const MachineRegisterInfo *MRI) const {
  // Check whether we can replace SUB with CMP.
  switch (CmpInstr.getOpcode()) {
  default: return false;
  case Z80::SUB8ai:
    // cp a,0 -> or a,a (a szhc have same behavior)
    // FIXME: This doesn't work if the pv flag is used.
    if (!CmpInstr.getOperand(0).getImm()) {
      CmpInstr.setDesc(get(Z80::OR8ar));
      CmpInstr.getOperand(0).ChangeToRegister(Z80::A, /*isDef=*/false);
      return true;
    }
    LLVM_FALLTHROUGH;
  case Z80::SUB8ar:
  case Z80::SUB8am:
  case Z80::SUB8ao: {
    if (!CmpInstr.registerDefIsDead(Z80::A))
      return false;
    // There is no use of the destination register, we can replace SUB with CMP.
    unsigned NewOpcode = 0;
    switch (CmpInstr.getOpcode()) {
    default: llvm_unreachable("Unreachable!");
    case Z80::SUB8ai: NewOpcode = Z80::CP8ai; break;
    case Z80::SUB8ar: NewOpcode = Z80::CP8ar; break;
    case Z80::SUB8am: NewOpcode = Z80::CP8am; break;
    case Z80::SUB8ao: NewOpcode = Z80::CP8ao; break;
    }
    CmpInstr.setDesc(get(NewOpcode));
    //CmpInstr.findRegisterDefOperand(Z80::A)->setIsDead(false);
    //BuildMI(*CmpInstr.getParent(), ++MachineBasicBlock::iterator(CmpInstr), CmpInstr.getDebugLoc(), get(TargetOpcode::COPY), SrcReg).addReg(Z80::A, RegState::Kill);
    return true;
  }
  }

  // Get the unique definition of SrcReg.
  MachineInstr *MI = MRI->getUniqueVRegDef(SrcReg);
  if (!MI) return false;

  // CmpInstr is the first instruction of the BB.
  MachineBasicBlock::iterator I = CmpInstr, Def = MI;

  // If we are comparing against zero, check whether we can use MI to update F.
  // If MI is not in the same BB as CmpInstr, do not optimize.
  bool IsCmpZero = (SrcReg2 == 0 && CmpValue == 0);
  if (IsCmpZero && MI->getParent() != CmpInstr.getParent())
    return false;

  llvm_unreachable("Unimplemented!");
}

MachineInstr *
Z80InstrInfo::foldMemoryOperandImpl(MachineFunction &MF, MachineInstr &MI,
                                    ArrayRef<unsigned> Ops,
                                    MachineBasicBlock::iterator InsertPt,
                                    int FrameIndex, LiveIntervals *LIS) const {
  return nullptr;
  bool Is24Bit = Subtarget.is24Bit();
  MachineBasicBlock &MBB = *InsertPt->getParent();
  if (Ops.size() == 1 && Ops[0] == 1 && MI.isFullCopy()) {
    unsigned DstReg = MI.getOperand(0).getReg();
    if (TargetRegisterInfo::isPhysicalRegister(DstReg)) {
      unsigned Opc;
      if (Z80::R8RegClass.contains(DstReg)) {
        Opc = Z80::LD8ro;
      } else {
        assert((Is24Bit ? Z80::R24RegClass : Z80::R16RegClass)
               .contains(DstReg) && "Unexpected physical reg");
        Opc = Is24Bit ? Z80::LD24ro : Z80::LD16ro;
      }
      return BuildMI(MBB, InsertPt, MI.getDebugLoc(), get(Opc), DstReg)
        .addFrameIndex(FrameIndex).addImm(0);
    }
  }
  dbgs() << Ops.size() << ": ";
  for (unsigned Op : Ops)
    dbgs() << Op << ' ';
  MI.dump();
  return nullptr;
}
MachineInstr *
Z80InstrInfo::foldMemoryOperandImpl(MachineFunction &MF, MachineInstr &MI,
                                    ArrayRef<unsigned> Ops,
                                    MachineBasicBlock::iterator InsertPt,
                                    MachineInstr &LoadMI,
                                    LiveIntervals *LIS) const {
  return nullptr;
  llvm_unreachable("Unimplemented");
}
