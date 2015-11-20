//===- subzero/src/IceAssemblerARM32.cpp - Assembler for ARM32 --*- C++ -*-===//
//
// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// Modified by the Subzero authors.
//
//===----------------------------------------------------------------------===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the Assembler class for ARM32.
///
//===----------------------------------------------------------------------===//

#include "IceAssemblerARM32.h"
#include "IceCfgNode.h"
#include "IceUtils.h"

namespace {

using namespace Ice;
using namespace Ice::ARM32;

using WordType = uint32_t;
static constexpr IValueT kWordSize = sizeof(WordType);

// The following define individual bits.
static constexpr IValueT B0 = 1;
static constexpr IValueT B1 = 1 << 1;
static constexpr IValueT B2 = 1 << 2;
static constexpr IValueT B3 = 1 << 3;
static constexpr IValueT B4 = 1 << 4;
static constexpr IValueT B5 = 1 << 5;
static constexpr IValueT B6 = 1 << 6;
static constexpr IValueT B7 = 1 << 7;
static constexpr IValueT B12 = 1 << 12;
static constexpr IValueT B13 = 1 << 13;
static constexpr IValueT B14 = 1 << 14;
static constexpr IValueT B15 = 1 << 15;
static constexpr IValueT B20 = 1 << 20;
static constexpr IValueT B21 = 1 << 21;
static constexpr IValueT B22 = 1 << 22;
static constexpr IValueT B23 = 1 << 23;
static constexpr IValueT B24 = 1 << 24;
static constexpr IValueT B25 = 1 << 25;
static constexpr IValueT B26 = 1 << 26;
static constexpr IValueT B27 = 1 << 27;

// Constants used for the decoding or encoding of the individual fields of
// instructions. Based on ARM section A5.1.
static constexpr IValueT L = 1 << 20; // load (or store)
static constexpr IValueT W = 1 << 21; // writeback base register
                                      // (or leave unchanged)
static constexpr IValueT B = 1 << 22; // unsigned byte (or word)
static constexpr IValueT U = 1 << 23; // positive (or negative)
                                      // offset/index
static constexpr IValueT P = 1 << 24; // offset/pre-indexed
                                      // addressing (or
                                      // post-indexed addressing)

static constexpr IValueT kConditionShift = 28;
static constexpr IValueT kLinkShift = 24;
static constexpr IValueT kOpcodeShift = 21;
static constexpr IValueT kRdShift = 12;
static constexpr IValueT kRmShift = 0;
static constexpr IValueT kRnShift = 16;
static constexpr IValueT kRsShift = 8;
static constexpr IValueT kSShift = 20;
static constexpr IValueT kTypeShift = 25;

// Immediate instruction fields encoding.
static constexpr IValueT kImmed8Bits = 8;
static constexpr IValueT kImmed8Shift = 0;
static constexpr IValueT kRotateBits = 4;
static constexpr IValueT kRotateShift = 8;

// Shift instruction register fields encodings.
static constexpr IValueT kShiftImmShift = 7;
static constexpr IValueT kShiftImmBits = 5;
static constexpr IValueT kShiftShift = 5;

static constexpr IValueT kImmed12Bits = 12;
static constexpr IValueT kImm12Shift = 0;

// Rotation instructions (uxtb etc.).
static constexpr IValueT kRotationShift = 10;

// Div instruction register field encodings.
static constexpr IValueT kDivRdShift = 16;
static constexpr IValueT kDivRmShift = 8;
static constexpr IValueT kDivRnShift = 0;

// Type of instruction encoding (bits 25-27). See ARM section A5.1
static constexpr IValueT kInstTypeDataRegister = 0;  // i.e. 000
static constexpr IValueT kInstTypeDataRegShift = 0;  // i.e. 000
static constexpr IValueT kInstTypeDataImmediate = 1; // i.e. 001
static constexpr IValueT kInstTypeMemImmediate = 2;  // i.e. 010
static constexpr IValueT kInstTypeRegisterShift = 3; // i.e. 011

// Offset modifier to current PC for next instruction.  The offset is off by 8
// due to the way the ARM CPUs read PC.
static constexpr IOffsetT kPCReadOffset = 8;

// Mask to pull out PC offset from branch (b) instruction.
static constexpr int kBranchOffsetBits = 24;
static constexpr IOffsetT kBranchOffsetMask = 0x00ffffff;

inline IValueT encodeBool(bool B) { return B ? 1 : 0; }

inline IValueT encodeRotation(ARM32::AssemblerARM32::RotationValue Value) {
  return static_cast<IValueT>(Value);
}

inline IValueT encodeGPRRegister(RegARM32::GPRRegister Rn) {
  return static_cast<IValueT>(Rn);
}

inline RegARM32::GPRRegister decodeGPRRegister(IValueT R) {
  return static_cast<RegARM32::GPRRegister>(R);
}

inline bool isGPRRegisterDefined(RegARM32::GPRRegister R) {
  return R != RegARM32::Encoded_Not_GPR;
}

inline bool isGPRRegisterDefined(IValueT R) {
  return R != encodeGPRRegister(RegARM32::Encoded_Not_GPR);
}

inline bool isConditionDefined(CondARM32::Cond Cond) {
  return Cond != CondARM32::kNone;
}

inline IValueT encodeCondition(CondARM32::Cond Cond) {
  return static_cast<IValueT>(Cond);
}

IValueT encodeShift(OperandARM32::ShiftKind Shift) {
  // Follows encoding in ARM section A8.4.1 "Constant shifts".
  switch (Shift) {
  case OperandARM32::kNoShift:
  case OperandARM32::LSL:
    return 0; // 0b00
  case OperandARM32::LSR:
    return 1; // 0b01
  case OperandARM32::ASR:
    return 2; // 0b10
  case OperandARM32::ROR:
  case OperandARM32::RRX:
    return 3; // 0b11
  }

  llvm::report_fatal_error("Unknown Shift value");
  return 0;
}

// Returns the bits in the corresponding masked value.
inline IValueT mask(IValueT Value, IValueT Shift, IValueT Bits) {
  return (Value >> Shift) & ((1 << Bits) - 1);
}

// Extract out a Bit in Value.
inline bool isBitSet(IValueT Bit, IValueT Value) {
  return (Value & Bit) == Bit;
}

// Returns the GPR register at given Shift in Value.
inline RegARM32::GPRRegister getGPRReg(IValueT Shift, IValueT Value) {
  return decodeGPRRegister((Value >> Shift) & 0xF);
}

// The way an operand was decoded in functions decodeOperand and decodeAddress
// below.
enum DecodedResult {
  // Unable to decode, value left undefined.
  CantDecode = 0,
  // Value is register found.
  DecodedAsRegister,
  // Value=rrrriiiiiiii where rrrr is the rotation, and iiiiiiii is the imm8
  // value.
  DecodedAsRotatedImm8,
  // Value=0000000pu0w0nnnn0000iiiiiiiiiiii where nnnn is the base register Rn,
  // p=1 if pre-indexed addressing, u=1 if offset positive, w=1 if writeback to
  // Rn should be used, and iiiiiiiiiiii defines the rotated Imm8 value.
  DecodedAsImmRegOffset,
  // Value=0000000pu0w00nnnnttttiiiiiss0mmmm where nnnn is the base register Rn,
  // mmmm is the index register Rm, iiiii is the shift amount, ss is the shift
  // kind, p=1 if pre-indexed addressing, u=1 if offset positive, and w=1 if
  // writeback to Rn.
  DecodedAsShiftRotateImm5,
  // Value=000000000000000000000iiiii0000000 iiii defines the Imm5 value to
  // shift.
  DecodedAsShiftImm5,
  // Value is 32bit integer constant.
  DecodedAsConstI32
};

// Sets Encoding to a rotated Imm8 encoding of Value, if possible.
inline IValueT encodeRotatedImm8(IValueT RotateAmt, IValueT Immed8) {
  assert(RotateAmt < (1 << kRotateBits));
  assert(Immed8 < (1 << kImmed8Bits));
  return (RotateAmt << kRotateShift) | (Immed8 << kImmed8Shift);
}

// Encodes iiiiitt0mmmm for data-processing (2nd) operands where iiiii=Imm5,
// tt=Shift, and mmmm=Rm.
IValueT encodeShiftRotateImm5(IValueT Rm, OperandARM32::ShiftKind Shift,
                              IOffsetT imm5) {
  (void)kShiftImmBits;
  assert(imm5 < (1 << kShiftImmBits));
  return (imm5 << kShiftImmShift) | (encodeShift(Shift) << kShiftShift) | Rm;
}

// Encodes mmmmtt01ssss for data-processing operands where mmmm=Rm, ssss=Rs, and
// tt=Shift.
IValueT encodeShiftRotateReg(IValueT Rm, OperandARM32::ShiftKind Shift,
                             IValueT Rs) {
  return (Rs << kRsShift) | (encodeShift(Shift) << kShiftShift) | B4 |
         (Rm << kRmShift);
}

DecodedResult decodeOperand(const Operand *Opnd, IValueT &Value) {
  if (const auto *Var = llvm::dyn_cast<Variable>(Opnd)) {
    if (Var->hasReg()) {
      Value = Var->getRegNum();
      return DecodedAsRegister;
    }
    return CantDecode;
  }
  if (const auto *FlexImm = llvm::dyn_cast<OperandARM32FlexImm>(Opnd)) {
    const IValueT Immed8 = FlexImm->getImm();
    const IValueT Rotate = FlexImm->getRotateAmt();
    if (!((Rotate < (1 << kRotateBits)) && (Immed8 < (1 << kImmed8Bits))))
      return CantDecode;
    Value = (Rotate << kRotateShift) | (Immed8 << kImmed8Shift);
    return DecodedAsRotatedImm8;
  }
  if (const auto *Const = llvm::dyn_cast<ConstantInteger32>(Opnd)) {
    Value = Const->getValue();
    return DecodedAsConstI32;
  }
  if (const auto *ShImm = llvm::dyn_cast<OperandARM32ShAmtImm>(Opnd)) {
    const IValueT Immed5 = ShImm->getShAmtImm();
    assert(Immed5 < (1 << kShiftImmBits));
    Value = (Immed5 << kShiftImmShift);
    return DecodedAsShiftImm5;
  }
  return CantDecode;
}

IValueT decodeImmRegOffset(RegARM32::GPRRegister Reg, IOffsetT Offset,
                           OperandARM32Mem::AddrMode Mode) {
  IValueT Value = Mode | (encodeGPRRegister(Reg) << kRnShift);
  if (Offset < 0) {
    Value = (Value ^ U) | -Offset; // Flip U to adjust sign.
  } else {
    Value |= Offset;
  }
  return Value;
}

// Decodes memory address Opnd, and encodes that information into Value,
// based on how ARM represents the address. Returns how the value was encoded.
DecodedResult decodeAddress(const Operand *Opnd, IValueT &Value,
                            const AssemblerARM32::TargetInfo &TInfo) {
  if (const auto *Var = llvm::dyn_cast<Variable>(Opnd)) {
    // Should be a stack variable, with an offset.
    if (Var->hasReg())
      return CantDecode;
    IOffsetT Offset = Var->getStackOffset();
    if (!Utils::IsAbsoluteUint(12, Offset))
      return CantDecode;
    int32_t BaseRegNum = Var->getBaseRegNum();
    if (BaseRegNum == Variable::NoRegister) {
      BaseRegNum = TInfo.FrameOrStackReg;
      if (!TInfo.HasFramePointer)
        Offset += TInfo.StackAdjustment;
    }
    Value = decodeImmRegOffset(decodeGPRRegister(BaseRegNum), Offset,
                               OperandARM32Mem::Offset);
    return DecodedAsImmRegOffset;
  }
  if (const auto *Mem = llvm::dyn_cast<OperandARM32Mem>(Opnd)) {
    Variable *Var = Mem->getBase();
    if (!Var->hasReg())
      return CantDecode;
    IValueT Rn = Var->getRegNum();
    if (Mem->isRegReg()) {
      const Variable *Index = Mem->getIndex();
      if (Var == nullptr)
        return CantDecode;
      Value = (Rn << kRnShift) | Mem->getAddrMode() |
              encodeShiftRotateImm5(Index->getRegNum(), Mem->getShiftOp(),
                                    Mem->getShiftAmt());
      return DecodedAsShiftRotateImm5;
    }
    // Decoded as immediate register offset.
    ConstantInteger32 *Offset = Mem->getOffset();
    Value = decodeImmRegOffset(decodeGPRRegister(Rn), Offset->getValue(),
                               Mem->getAddrMode());
    return DecodedAsImmRegOffset;
  }
  return CantDecode;
}

// Checks that Offset can fit in imm24 constant of branch (b) instruction.
bool canEncodeBranchOffset(IOffsetT Offset) {
  return Utils::IsAligned(Offset, 4) &&
         Utils::IsInt(kBranchOffsetBits, Offset >> 2);
}

} // end of anonymous namespace

namespace Ice {
namespace ARM32 {

size_t MoveRelocatableFixup::emit(GlobalContext *Ctx,
                                  const Assembler &Asm) const {
  if (!BuildDefs::dump())
    return InstARM32::InstSize;
  Ostream &Str = Ctx->getStrEmit();
  IValueT Inst = Asm.load<IValueT>(position());
  Str << "\tmov" << (kind() == llvm::ELF::R_ARM_MOVW_ABS_NC ? "w" : "t") << "\t"
      << RegARM32::RegNames[(Inst >> kRdShift) & 0xF]
      << ", #:" << (kind() == llvm::ELF::R_ARM_MOVW_ABS_NC ? "lower" : "upper")
      << "16:" << symbol(Ctx) << "\t@ .word "
      << llvm::format_hex_no_prefix(Inst, 8) << "\n";
  return InstARM32::InstSize;
}

MoveRelocatableFixup *AssemblerARM32::createMoveFixup(bool IsMovW,
                                                      const Constant *Value) {
  MoveRelocatableFixup *F =
      new (allocate<MoveRelocatableFixup>()) MoveRelocatableFixup();
  F->set_kind(IsMovW ? llvm::ELF::R_ARM_MOVW_ABS_NC
                     : llvm::ELF::R_ARM_MOVT_ABS);
  F->set_value(Value);
  Buffer.installFixup(F);
  return F;
}

size_t BlRelocatableFixup::emit(GlobalContext *Ctx,
                                const Assembler &Asm) const {
  if (!BuildDefs::dump())
    return InstARM32::InstSize;
  Ostream &Str = Ctx->getStrEmit();
  IValueT Inst = Asm.load<IValueT>(position());
  Str << "\tbl\t" << symbol(Ctx) << "\t@ .word "
      << llvm::format_hex_no_prefix(Inst, 8) << "\n";
  return InstARM32::InstSize;
}

BlRelocatableFixup *
AssemblerARM32::createBlFixup(const ConstantRelocatable *Target) {
  BlRelocatableFixup *F =
      new (allocate<BlRelocatableFixup>()) BlRelocatableFixup();
  F->set_kind(llvm::ELF::R_ARM_CALL);
  F->set_value(Target);
  Buffer.installFixup(F);
  return F;
}

void AssemblerARM32::bindCfgNodeLabel(const CfgNode *Node) {
  GlobalContext *Ctx = Node->getCfg()->getContext();
  if (BuildDefs::dump() && !Ctx->getFlags().getDisableHybridAssembly()) {
    // Generate label name so that branches can find it.
    constexpr SizeT InstSize = 0;
    emitTextInst(Node->getAsmName() + ":", InstSize);
  }
  SizeT NodeNumber = Node->getIndex();
  assert(!getPreliminary());
  Label *L = getOrCreateCfgNodeLabel(NodeNumber);
  this->bind(L);
}

Label *AssemblerARM32::getOrCreateLabel(SizeT Number, LabelVector &Labels) {
  Label *L = nullptr;
  if (Number == Labels.size()) {
    L = new (this->allocate<Label>()) Label();
    Labels.push_back(L);
    return L;
  }
  if (Number > Labels.size()) {
    Labels.resize(Number + 1);
  }
  L = Labels[Number];
  if (!L) {
    L = new (this->allocate<Label>()) Label();
    Labels[Number] = L;
  }
  return L;
}

IValueT AssemblerARM32::encodeBranchOffset(IOffsetT Offset, IValueT Inst) {
  // Adjust offset to the way ARM CPUs read PC.
  Offset -= kPCReadOffset;

  bool IsGoodOffset = canEncodeBranchOffset(Offset);
  assert(IsGoodOffset);
  // Note: Following cast is for MINIMAL build.
  (void)IsGoodOffset;

  // Properly preserve only the bits supported in the instruction.
  Offset >>= 2;
  Offset &= kBranchOffsetMask;
  return (Inst & ~kBranchOffsetMask) | Offset;
}

IOffsetT AssemblerARM32::decodeBranchOffset(IValueT Inst) {
  // Sign-extend, left-shift by 2, and adjust to the way ARM CPUs read PC.
  IOffsetT Offset = static_cast<IOffsetT>((Inst & kBranchOffsetMask) << 8);
  return (Offset >> 6) + kPCReadOffset;
}

void AssemblerARM32::bind(Label *L) {
  IOffsetT BoundPc = Buffer.size();
  assert(!L->isBound()); // Labels can only be bound once.
  while (L->isLinked()) {
    IOffsetT Position = L->getLinkPosition();
    IOffsetT Dest = BoundPc - Position;
    IValueT Inst = Buffer.load<IValueT>(Position);
    Buffer.store<IValueT>(Position, encodeBranchOffset(Dest, Inst));
    L->setPosition(decodeBranchOffset(Inst));
  }
  L->bindTo(BoundPc);
}

void AssemblerARM32::emitTextInst(const std::string &Text, SizeT InstSize) {
  AssemblerFixup *F = createTextFixup(Text, InstSize);
  emitFixup(F);
  for (SizeT I = 0; I < InstSize; ++I) {
    AssemblerBuffer::EnsureCapacity ensured(&Buffer);
    Buffer.emit<char>(0);
  }
}

void AssemblerARM32::emitType01(CondARM32::Cond Cond, IValueT Type,
                                IValueT Opcode, bool SetFlags, IValueT Rn,
                                IValueT Rd, IValueT Imm12,
                                EmitChecks RuleChecks) {
  switch (RuleChecks) {
  case NoChecks:
    break;
  case RdIsPcAndSetFlags:
    if (((Rd == RegARM32::Encoded_Reg_pc) && SetFlags))
      // Conditions of rule violated.
      return setNeedsTextFixup();
    break;
  }

  if (!isGPRRegisterDefined(Rd) || !isConditionDefined(Cond))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  const IValueT Encoding = (encodeCondition(Cond) << kConditionShift) |
                           (Type << kTypeShift) | (Opcode << kOpcodeShift) |
                           (encodeBool(SetFlags) << kSShift) |
                           (Rn << kRnShift) | (Rd << kRdShift) | Imm12;
  emitInst(Encoding);
}

void AssemblerARM32::emitType01(IValueT Opcode, const Operand *OpRd,
                                const Operand *OpRn, const Operand *OpSrc1,
                                bool SetFlags, CondARM32::Cond Cond,
                                EmitChecks RuleChecks) {
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rn;
  if (decodeOperand(OpRn, Rn) != DecodedAsRegister)
    return setNeedsTextFixup();
  emitType01(Opcode, Rd, Rn, OpSrc1, SetFlags, Cond, RuleChecks);
}

void AssemblerARM32::emitType01(IValueT Opcode, IValueT Rd, IValueT Rn,
                                const Operand *OpSrc1, bool SetFlags,
                                CondARM32::Cond Cond, EmitChecks RuleChecks) {

  IValueT Src1Value;
  // TODO(kschimpf) Other possible decodings of data operations.
  switch (decodeOperand(OpSrc1, Src1Value)) {
  default:
    return setNeedsTextFixup();
  case DecodedAsRegister: {
    // XXX (register)
    //   xxx{s}<c> <Rd>, <Rn>, <Rm>{, <shiff>}
    //
    // cccc0000100snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
    // mmmm=Rm, iiiii=Shift, tt=ShiftKind, and s=SetFlags.
    constexpr IValueT Imm5 = 0;
    Src1Value = encodeShiftRotateImm5(Src1Value, OperandARM32::kNoShift, Imm5);
    emitType01(Cond, kInstTypeDataRegister, Opcode, SetFlags, Rn, Rd, Src1Value,
               RuleChecks);
    return;
  }
  case DecodedAsConstI32: {
    // See if we can convert this to an XXX (immediate).
    IValueT RotateAmt;
    IValueT Imm8;
    if (!OperandARM32FlexImm::canHoldImm(Src1Value, &RotateAmt, &Imm8))
      return setNeedsTextFixup();
    Src1Value = encodeRotatedImm8(RotateAmt, Imm8);
    // Intentionally fall to next case!
  }
  case DecodedAsRotatedImm8: {
    // XXX (Immediate)
    //   xxx{s}<c> <Rd>, <Rn>, #<RotatedImm8>
    //
    // cccc0010100snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
    // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8.
    emitType01(Cond, kInstTypeDataImmediate, Opcode, SetFlags, Rn, Rd,
               Src1Value, RuleChecks);
    return;
  }
  }
}

void AssemblerARM32::emitType05(CondARM32::Cond Cond, IOffsetT Offset,
                                bool Link) {
  // cccc101liiiiiiiiiiiiiiiiiiiiiiii where cccc=Cond, l=Link, and
  // iiiiiiiiiiiiiiiiiiiiiiii=
  // EncodedBranchOffset(cccc101l000000000000000000000000, Offset);
  if (!isConditionDefined(Cond))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  IValueT Encoding = static_cast<int32_t>(Cond) << kConditionShift |
                     5 << kTypeShift | (Link ? 1 : 0) << kLinkShift;
  Encoding = encodeBranchOffset(Offset, Encoding);
  emitInst(Encoding);
}

void AssemblerARM32::emitBranch(Label *L, CondARM32::Cond Cond, bool Link) {
  // TODO(kschimpf): Handle far jumps.
  if (L->isBound()) {
    const int32_t Dest = L->getPosition() - Buffer.size();
    emitType05(Cond, Dest, Link);
    return;
  }
  const IOffsetT Position = Buffer.size();
  // Use the offset field of the branch instruction for linking the sites.
  emitType05(Cond, L->getEncodedPosition(), Link);
  if (!needsTextFixup())
    L->linkTo(Position);
}

void AssemblerARM32::emitCompareOp(IValueT Opcode, const Operand *OpRn,
                                   const Operand *OpSrc1,
                                   CondARM32::Cond Cond) {
  // XXX (register)
  //   XXX<c> <Rn>, <Rm>{, <shift>}
  //
  // ccccyyyxxxx1nnnn0000iiiiitt0mmmm where cccc=Cond, nnnn=Rn, mmmm=Rm, iiiii
  // defines shift constant, tt=ShiftKind, yyy=kInstTypeDataRegister, and
  // xxxx=Opcode.
  //
  // XXX (immediate)
  //  XXX<c> <Rn>, #<RotatedImm8>
  //
  // ccccyyyxxxx1nnnn0000iiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // yyy=kInstTypeDataImmdiate, xxxx=Opcode, and iiiiiiiiiiii=Src1Value
  // defining RotatedImm8.
  constexpr bool SetFlags = true;
  constexpr IValueT Rd = RegARM32::Encoded_Reg_r0;
  IValueT Rn;
  if (decodeOperand(OpRn, Rn) != DecodedAsRegister)
    return setNeedsTextFixup();
  emitType01(Opcode, Rd, Rn, OpSrc1, SetFlags, Cond, NoChecks);
}

void AssemblerARM32::emitMemOp(CondARM32::Cond Cond, IValueT InstType,
                               bool IsLoad, bool IsByte, IValueT Rt,
                               IValueT Address) {
  if (!isGPRRegisterDefined(Rt) || !isConditionDefined(Cond))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  const IValueT Encoding = (encodeCondition(Cond) << kConditionShift) |
                           (InstType << kTypeShift) | (IsLoad ? L : 0) |
                           (IsByte ? B : 0) | (Rt << kRdShift) | Address;
  emitInst(Encoding);
}

void AssemblerARM32::emitDivOp(CondARM32::Cond Cond, IValueT Opcode, IValueT Rd,
                               IValueT Rn, IValueT Rm) {
  if (!isGPRRegisterDefined(Rd) || !isGPRRegisterDefined(Rn) ||
      !isGPRRegisterDefined(Rm) || !isConditionDefined(Cond))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  const IValueT Encoding = Opcode | (encodeCondition(Cond) << kConditionShift) |
                           (Rn << kDivRnShift) | (Rd << kDivRdShift) | B26 |
                           B25 | B24 | B20 | B15 | B14 | B13 | B12 | B4 |
                           (Rm << kDivRmShift);
  emitInst(Encoding);
}

void AssemblerARM32::emitMulOp(CondARM32::Cond Cond, IValueT Opcode, IValueT Rd,
                               IValueT Rn, IValueT Rm, IValueT Rs,
                               bool SetFlags) {
  if (!isGPRRegisterDefined(Rd) || !isGPRRegisterDefined(Rn) ||
      !isGPRRegisterDefined(Rm) || !isGPRRegisterDefined(Rs) ||
      !isConditionDefined(Cond))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  IValueT Encoding = Opcode | (encodeCondition(Cond) << kConditionShift) |
                     (encodeBool(SetFlags) << kSShift) | (Rn << kRnShift) |
                     (Rd << kRdShift) | (Rs << kRsShift) | B7 | B4 |
                     (Rm << kRmShift);
  emitInst(Encoding);
}

void AssemblerARM32::emitUxt(CondARM32::Cond Cond, IValueT Opcode, IValueT Rd,
                             IValueT Rn, IValueT Rm, RotationValue Rotation) {
  IValueT Rot = encodeRotation(Rotation);
  if (!isConditionDefined(Cond) || !Utils::IsUint(2, Rot))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  IValueT Encoding = (encodeCondition(Cond) << kConditionShift) | Opcode |
                     (Rn << kRnShift) | (Rd << kRdShift) |
                     (Rot << kRotationShift) | B6 | B5 | B4 | (Rm << kRmShift);
  emitInst(Encoding);
}

void AssemblerARM32::emitMultiMemOp(CondARM32::Cond Cond,
                                    BlockAddressMode AddressMode, bool IsLoad,
                                    IValueT BaseReg, IValueT Registers) {
  constexpr IValueT NumGPRegisters = 16;
  if (!isConditionDefined(Cond) || !isGPRRegisterDefined(BaseReg) ||
      Registers >= (1 << NumGPRegisters))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  IValueT Encoding = (encodeCondition(Cond) << kConditionShift) | B27 |
                     AddressMode | (IsLoad ? L : 0) | (BaseReg << kRnShift) |
                     Registers;
  emitInst(Encoding);
}

void AssemblerARM32::adc(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  // ADC (register) - ARM section 18.8.2, encoding A1:
  //   adc{s}<c> <Rd>, <Rn>, <Rm>{, <shift>}
  //
  // cccc0000101snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, iiiii=Shift, tt=ShiftKind, and s=SetFlags.
  //
  // ADC (Immediate) - ARM section A8.8.1, encoding A1:
  //   adc{s}<c> <Rd>, <Rn>, #<RotatedImm8>
  //
  // cccc0010101snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8.
  constexpr IValueT Adc = B2 | B0; // 0101
  emitType01(Adc, OpRd, OpRn, OpSrc1, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::add(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  // ADD (register) - ARM section A8.8.7, encoding A1:
  //   add{s}<c> <Rd>, <Rn>, <Rm>{, <shiff>}
  // ADD (Sp plus register) - ARM section A8.8.11, encoding A1:
  //   add{s}<c> sp, <Rn>, <Rm>{, <shiff>}
  //
  // cccc0000100snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, iiiii=Shift, tt=ShiftKind, and s=SetFlags.
  //
  // ADD (Immediate) - ARM section A8.8.5, encoding A1:
  //   add{s}<c> <Rd>, <Rn>, #<RotatedImm8>
  // ADD (SP plus immediate) - ARM section A8.8.9, encoding A1.
  //   add{s}<c> <Rd>, sp, #<RotatedImm8>
  //
  // cccc0010100snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8.
  constexpr IValueT Add = B2; // 0100
  emitType01(Add, OpRd, OpRn, OpSrc1, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::and_(const Operand *OpRd, const Operand *OpRn,
                          const Operand *OpSrc1, bool SetFlags,
                          CondARM32::Cond Cond) {
  // AND (register) - ARM section A8.8.14, encoding A1:
  //   and{s}<c> <Rd>, <Rn>{, <shift>}
  //
  // cccc0000000snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, iiiii=Shift, tt=ShiftKind, and s=SetFlags.
  //
  // AND (Immediate) - ARM section A8.8.13, encoding A1:
  //   and{s}<c> <Rd>, <Rn>, #<RotatedImm8>
  //
  // cccc0010100snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8.
  constexpr IValueT And = 0; // 0000
  emitType01(And, OpRd, OpRn, OpSrc1, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::b(Label *L, CondARM32::Cond Cond) {
  emitBranch(L, Cond, false);
}

void AssemblerARM32::bkpt(uint16_t Imm16) {
  // BKPT - ARM section A*.8.24 - encoding A1:
  //   bkpt #<Imm16>
  //
  // cccc00010010iiiiiiiiiiii0111iiii where cccc=AL and iiiiiiiiiiiiiiii=Imm16
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  const IValueT Encoding = (CondARM32::AL << kConditionShift) | B24 | B21 |
                           ((Imm16 >> 4) << 8) | B6 | B5 | B4 | (Imm16 & 0xf);
  emitInst(Encoding);
}

void AssemblerARM32::bic(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  // BIC (register) - ARM section A8.8.22, encoding A1:
  //   bic{s}<c> <Rd>, <Rn>, <Rm>{, <shift>}
  //
  // cccc0001110snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, iiiii=Shift, tt=ShiftKind, and s=SetFlags.
  //
  // BIC (immediate) - ARM section A8.8.21, encoding A1:
  //   bic{s}<c> <Rd>, <Rn>, #<RotatedImm8>
  //
  // cccc0011110snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rn, nnnn=Rn,
  // s=SetFlags, and iiiiiiiiiiii=Src1Value defining RotatedImm8.
  IValueT Opcode = B3 | B2 | B1; // i.e. 1110
  emitType01(Opcode, OpRd, OpRn, OpSrc1, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::bl(const ConstantRelocatable *Target) {
  // BL (immediate) - ARM section A8.8.25, encoding A1:
  //   bl<c> <label>
  //
  // cccc1011iiiiiiiiiiiiiiiiiiiiiiii where cccc=Cond (not currently allowed)
  // and iiiiiiiiiiiiiiiiiiiiiiii is the (encoded) Target to branch to.
  emitFixup(createBlFixup(Target));
  constexpr CondARM32::Cond Cond = CondARM32::AL;
  constexpr IValueT Immed = 0;
  constexpr bool Link = true;
  emitType05(Cond, Immed, Link);
}

void AssemblerARM32::blx(const Operand *Target) {
  IValueT Rm;
  if (decodeOperand(Target, Rm) != DecodedAsRegister)
    return setNeedsTextFixup();
  // BLX (register) - ARM section A8.8.26, encoding A1:
  //   blx<c> <Rm>
  //
  // cccc000100101111111111110011mmmm where cccc=Cond (not currently allowed)
  // and mmmm=Rm.
  if (Rm == RegARM32::Encoded_Reg_pc)
    // Unpredictable.
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  constexpr CondARM32::Cond Cond = CondARM32::AL;
  int32_t Encoding = (static_cast<int32_t>(Cond) << kConditionShift) | B24 |
                     B21 | (0xfff << 8) | B5 | B4 | (Rm << kRmShift);
  emitInst(Encoding);
}

void AssemblerARM32::bx(RegARM32::GPRRegister Rm, CondARM32::Cond Cond) {
  // BX - ARM section A8.8.27, encoding A1:
  //   bx<c> <Rm>
  //
  // cccc000100101111111111110001mmmm where mmmm=rm and cccc=Cond.
  if (!(isGPRRegisterDefined(Rm) && isConditionDefined(Cond)))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  const IValueT Encoding = (encodeCondition(Cond) << kConditionShift) | B24 |
                           B21 | (0xfff << 8) | B4 |
                           (encodeGPRRegister(Rm) << kRmShift);
  emitInst(Encoding);
}

void AssemblerARM32::cmp(const Operand *OpRn, const Operand *OpSrc1,
                         CondARM32::Cond Cond) {
  // CMP (register) - ARM section A8.8.38, encoding A1:
  //   cmp<c> <Rn>, <Rm>{, <shift>}
  //
  // cccc00010101nnnn0000iiiiitt0mmmm where cccc=Cond, nnnn=Rn, mmmm=Rm,
  // iiiii=Shift, and tt=ShiftKind.
  //
  // CMP (immediate) - ARM section A8.8.37
  //  cmp<c: <Rn>, #<RotatedImm8>
  //
  // cccc00110101nnnn0000iiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8.
  constexpr IValueT Opcode = B3 | B1; // ie. 1010
  emitCompareOp(Opcode, OpRn, OpSrc1, Cond);
}

void AssemblerARM32::eor(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  // EOR (register) - ARM section A*.8.47, encoding A1:
  //   eor{s}<c> <Rd>, <Rn>, <Rm>{, <shift>}
  //
  // cccc0000001snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, iiiii=Shift, tt=ShiftKind, and s=SetFlags.
  //
  // EOR (Immediate) - ARM section A8.*.46, encoding A1:
  //   eor{s}<c> <Rd>, <Rn>, #RotatedImm8
  //
  // cccc0010001snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8.
  constexpr IValueT Eor = B0; // 0001
  emitType01(Eor, OpRd, OpRn, OpSrc1, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::ldr(const Operand *OpRt, const Operand *OpAddress,
                         CondARM32::Cond Cond, const TargetInfo &TInfo) {
  constexpr bool IsLoad = true;
  IValueT Rt;
  if (decodeOperand(OpRt, Rt) != DecodedAsRegister)
    return setNeedsTextFixup();
  const Type Ty = OpRt->getType();
  if (!(Ty == IceType_i32 || Ty == IceType_i8)) // TODO(kschimpf) Expand?
    return setNeedsTextFixup();
  const bool IsByte = isByteSizedType(Ty);
  IValueT Address;
  switch (decodeAddress(OpAddress, Address, TInfo)) {
  default:
    return setNeedsTextFixup();
  case DecodedAsImmRegOffset: {
    // LDR (immediate) - ARM section A8.8.63, encoding A1:
    //   ldr<c> <Rt>, [<Rn>{, #+/-<imm12>}]      ; p=1, w=0
    //   ldr<c> <Rt>, [<Rn>], #+/-<imm12>        ; p=1, w=1
    //   ldr<c> <Rt>, [<Rn>, #+/-<imm12>]!       ; p=0, w=1
    //
    // LDRB (immediate) - ARM section A8.8.68, encoding A1:
    //   ldrb<c> <Rt>, [<Rn>{, #+/-<imm12>}]     ; p=1, w=0
    //   ldrb<c> <Rt>, [<Rn>], #+/-<imm12>       ; p=1, w=1
    //   ldrb<c> <Rt>, [<Rn>, #+/-<imm12>]!      ; p=0, w=1
    //
    // cccc010pubw1nnnnttttiiiiiiiiiiii where cccc=Cond, tttt=Rt, nnnn=Rn,
    // iiiiiiiiiiii=imm12, b=1 if LDRB, u=1 if +, pu0w is a BlockAddr, and
    // pu0w0nnnn0000iiiiiiiiiiii=Address.

    RegARM32::GPRRegister Rn = getGPRReg(kRnShift, Address);

    // Check if conditions of rules violated.
    if (Rn == RegARM32::Encoded_Reg_pc)
      return setNeedsTextFixup();
    if (!isBitSet(P, Address) && isBitSet(W, Address))
      return setNeedsTextFixup();
    if (!IsByte && (Rn == RegARM32::Encoded_Reg_sp) && !isBitSet(P, Address) &&
        isBitSet(U, Address) & !isBitSet(W, Address) &&
        (mask(Address, kImm12Shift, kImmed12Bits) == 0x8 /* 000000000100 */))
      return setNeedsTextFixup();

    return emitMemOp(Cond, kInstTypeMemImmediate, IsLoad, IsByte, Rt, Address);
  }
  case DecodedAsShiftRotateImm5: {
    // LDR (register) - ARM section A8.8.66, encoding A1:
    //   ldr<c> <Rt>, [<Rn>, +/-<Rm>{, <shift>}]{!}
    //   ldr<c> <Rt>, [<Rn>], +/-<Rm>{, <shift>}
    //
    // LDRB (register) - ARM section A8.8.70, encoding A1:
    //   ldrb<c> <Rt>, [<Rn>, +/-<Rm>{, <shift>}]{!}
    //   ldrb<c> <Rt>, [<Rn>], +-<Rm>{, <shift>}
    //
    // cccc011pubw1nnnnttttiiiiiss0mmmm where cccc=Cond, tttt=Rt,
    // b=1 if LDRB, U=1 if +, pu0b is a BlockAddr, and
    // pu0w0nnnn0000iiiiiss0mmmm=Address.

    RegARM32::GPRRegister Rn = getGPRReg(kRnShift, Address);
    RegARM32::GPRRegister Rm = getGPRReg(kRmShift, Address);

    // Check if conditions of rules violated.
    if (isBitSet(P, Address) && isBitSet(W, Address))
      // Instruction LDRBT!
      return setNeedsTextFixup();
    if (IsByte &&
        ((Rt == RegARM32::Encoded_Reg_pc) || (Rm == RegARM32::Encoded_Reg_pc)))
      // Unpredictable.
      return setNeedsTextFixup();
    if (!IsByte && Rm == RegARM32::Encoded_Reg_pc)
      // Unpredictable.
      return setNeedsTextFixup();
    if (isBitSet(W, Address) &&
        ((Rn == RegARM32::Encoded_Reg_pc) || encodeGPRRegister(Rn) == Rt))
      // Unpredictable
      return setNeedsTextFixup();

    return emitMemOp(Cond, kInstTypeRegisterShift, IsLoad, IsByte, Rt, Address);
  }
  }
}

void AssemblerARM32::lsl(const Operand *OpRd, const Operand *OpRm,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  constexpr IValueT Lsl = B3 | B2 | B0; // 1101
  constexpr IValueT Rn = 0;             // Rn field is not used.
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rm;
  if (decodeOperand(OpRm, Rm) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Value;
  switch (decodeOperand(OpSrc1, Value)) {
  default:
    return setNeedsTextFixup();
  case DecodedAsShiftImm5: {
    // LSL (immediate) - ARM section A8.8.94, encoding A1:
    //   lsl{s}<c> <Rd>, <Rm>, #imm5
    //
    // cccc0001101s0000ddddiiiii000mmmm where cccc=Cond, s=SetFlags, dddd=Rd,
    // iiiii=imm5, and mmmm=Rm.
    Value = Value | (Rm << kRmShift);
    emitType01(Cond, kInstTypeDataRegShift, Lsl, SetFlags, Rn, Rd, Value,
               RdIsPcAndSetFlags);
    return;
  }
  case DecodedAsRegister: {
    // LSL (register) - ARM section A8.8.95, encoding A1:
    //   lsl{S}<c> <Rd>, <Rm>, <Rs>
    //
    // cccc0001101s0000ddddssss0001mmmm where cccc=Cond, s=SetFlags, dddd=Rd,
    // mmmm=Rm, and ssss=Rs.
    IValueT Rs;
    if (decodeOperand(OpSrc1, Rs) != DecodedAsRegister)
      return setNeedsTextFixup();
    if ((Rd == RegARM32::Encoded_Reg_pc) || (Rm == RegARM32::Encoded_Reg_pc) ||
        (Rs == RegARM32::Encoded_Reg_pc))
      setNeedsTextFixup();
    emitType01(Cond, kInstTypeDataRegShift, Lsl, SetFlags, Rn, Rd,
               encodeShiftRotateReg(Rm, OperandARM32::kNoShift, Rs), NoChecks);
    return;
  }
  }
}

void AssemblerARM32::mov(const Operand *OpRd, const Operand *OpSrc,
                         CondARM32::Cond Cond) {
  // MOV (register) - ARM section A8.8.104, encoding A1:
  //   mov{S}<c> <Rd>, <Rn>
  //
  // cccc0001101s0000dddd00000000mmmm where cccc=Cond, s=SetFlags, dddd=Rd,
  // and nnnn=Rn.
  //
  // MOV (immediate) - ARM section A8.8.102, encoding A1:
  //   mov{S}<c> <Rd>, #<RotatedImm8>
  //
  // cccc0011101s0000ddddiiiiiiiiiiii where cccc=Cond, s=SetFlags, dddd=Rd,
  // and iiiiiiiiiiii=RotatedImm8=Src.  Note: We don't use movs in this
  // assembler.
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  constexpr bool SetFlags = false;
  constexpr IValueT Rn = 0;
  constexpr IValueT Mov = B3 | B2 | B0; // 1101.
  emitType01(Mov, Rd, Rn, OpSrc, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::emitMovw(IValueT Opcode, IValueT Rd, IValueT Imm16,
                              bool SetFlags, CondARM32::Cond Cond) {
  if (!isConditionDefined(Cond) || !Utils::IsAbsoluteUint(16, Imm16))
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  const IValueT Encoding = encodeCondition(Cond) << kConditionShift | Opcode |
                           (encodeBool(SetFlags) << kSShift) |
                           ((Imm16 >> 12) << 16) | Rd << kRdShift |
                           (Imm16 & 0xfff);
  emitInst(Encoding);
}

void AssemblerARM32::movw(const Operand *OpRd, const Operand *OpSrc,
                          CondARM32::Cond Cond) {
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  if (const auto *Src = llvm::dyn_cast<ConstantRelocatable>(OpSrc)) {
    // MOVW (immediate) - ARM section A8.8.102, encoding A2:
    //  movw<c> <Rd>, #<imm16>
    //
    // cccc00110000iiiiddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, and
    // iiiiiiiiiiiiiiii=imm16.
    if (!isConditionDefined(Cond))
      // Conditions of rule violated.
      return setNeedsTextFixup();
    // Use 0 for the lower 16 bits of the relocatable, and add a fixup to
    // install the correct bits.
    constexpr bool IsMovW = true;
    emitFixup(createMoveFixup(IsMovW, Src));
    constexpr IValueT Imm16 = 0;
    constexpr bool SetFlags = false;
    emitMovw(B25 | B24, Rd, Imm16, SetFlags, Cond);
    return;
  }
  IValueT ConstVal;
  if (decodeOperand(OpSrc, ConstVal) != DecodedAsConstI32)
    return setNeedsTextFixup();
  // TODO(kschimpf): Determine if we want to handle rotated immediate 8 values
  // to handle cases where the constant is greater than 16 bits (encoding A1
  // below).  For now, handle using encoding A2.
  constexpr bool SetFlags = 0;
  emitMovw(B25 | B24, Rd, ConstVal, SetFlags, Cond);
  return;

  // MOVW (immediate) - ARM section A8.8.102, encoding A1:
  //   movw<c> <Rd>, #<RotatedImm8>
  //
  // cccc0011101s0000ddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, s=SetFlags=0,
  // and iiiiiiiiiiii is a shift-rotated value defining RotatedImm8.
}

void AssemblerARM32::movt(const Operand *OpRd, const Operand *OpSrc,
                          CondARM32::Cond Cond) {
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  auto *Src = llvm::dyn_cast<ConstantRelocatable>(OpSrc);
  if (Src == nullptr)
    return setNeedsTextFixup();
  // MOVT - ARM section A8.8.102, encoding A2:
  //  movt<c> <Rd>, #<imm16>
  //
  // cccc00110100iiiiddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, and
  // iiiiiiiiiiiiiiii=imm16.
  if (!isConditionDefined(Cond))
    // Conditions of rule violated.
    return setNeedsTextFixup();
  AssemblerBuffer::EnsureCapacity ensured(&Buffer);
  // Use 0 for the lower 16 bits of the relocatable, and add a fixup to
  // install the correct bits.
  constexpr bool IsMovW = false;
  emitFixup(createMoveFixup(IsMovW, Src));
  constexpr IValueT Imm16 = 0;
  const IValueT Encoding = encodeCondition(Cond) << kConditionShift | B25 |
                           B24 | B22 | ((Imm16 >> 12) << 16) | Rd << kRdShift |
                           (Imm16 & 0xfff);
  emitInst(Encoding);
}

void AssemblerARM32::mvn(const Operand *OpRd, const Operand *OpSrc,
                         CondARM32::Cond Cond) {
  // MVN (immediate) - ARM section A8.8.115, encoding A1:
  //   mvn{s}<c> <Rd>, #<const>
  //
  // cccc0011111s0000ddddiiiiiiiiiiii where cccc=Cond, s=SetFlags=0, dddd=Rd,
  // and iiiiiiiiiiii=const
  //
  // MVN (register) - ARM section A8.8.116, encoding A1:
  //   mvn{s}<c> <Rd>, <Rm>{, <shift>
  //
  // cccc0001111s0000ddddiiiiitt0mmmm where cccc=Cond, s=SetFlags=0, dddd=Rd,
  // mmmm=Rm, iiii defines shift constant, and tt=ShiftKind.
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  constexpr IValueT MvnOpcode = B3 | B2 | B1 | B0; // i.e. 1111
  constexpr IValueT Rn = 0;
  constexpr bool SetFlags = false;
  emitType01(MvnOpcode, Rd, Rn, OpSrc, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::sbc(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  // SBC (register) - ARM section 18.8.162, encoding A1:
  //   sbc{s}<c> <Rd>, <Rn>, <Rm>{, <shift>}
  //
  // cccc0000110snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, iiiii=Shift, tt=ShiftKind, and s=SetFlags.
  //
  // SBC (Immediate) - ARM section A8.8.161, encoding A1:
  //   sbc{s}<c> <Rd>, <Rn>, #<RotatedImm8>
  //
  // cccc0010110snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8.
  constexpr IValueT Sbc = B2 | B1; // 0110
  emitType01(Sbc, OpRd, OpRn, OpSrc1, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::sdiv(const Operand *OpRd, const Operand *OpRn,
                          const Operand *OpSrc1, CondARM32::Cond Cond) {
  // SDIV - ARM section A8.8.165, encoding A1.
  //   sdiv<c> <Rd>, <Rn>, <Rm>
  //
  // cccc01110001dddd1111mmmm0001nnnn where cccc=Cond, dddd=Rd, nnnn=Rn, and
  // mmmm=Rm.
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rn;
  if (decodeOperand(OpRn, Rn) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rm;
  if (decodeOperand(OpSrc1, Rm) != DecodedAsRegister)
    return setNeedsTextFixup();
  if (Rd == RegARM32::Encoded_Reg_pc || Rn == RegARM32::Encoded_Reg_pc ||
      Rm == RegARM32::Encoded_Reg_pc)
    llvm::report_fatal_error("Sdiv instruction unpredictable on pc");
  // Assembler registers rd, rn, rm are encoded as rn, rm, rs.
  constexpr IValueT Opcode = 0;
  emitDivOp(Cond, Opcode, Rd, Rn, Rm);
}

void AssemblerARM32::str(const Operand *OpRt, const Operand *OpAddress,
                         CondARM32::Cond Cond, const TargetInfo &TInfo) {
  IValueT Rt;
  if (decodeOperand(OpRt, Rt) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Address;
  if (decodeAddress(OpAddress, Address, TInfo) != DecodedAsImmRegOffset)
    return setNeedsTextFixup();
  // STR (immediate) - ARM section A8.8.204, encoding A1:
  //   str<c> <Rt>, [<Rn>{, #+/-<imm12>}]      ; p=1, w=0
  //   str<c> <Rt>, [<Rn>], #+/-<imm12>        ; p=1, w=1
  //   str<c> <Rt>, [<Rn>, #+/-<imm12>]!       ; p=0, w=1
  // STRB (immediate) - ARM section A8.8.207, encoding A1:
  //   strb<c> <Rt>, [<Rn>{, #+/-<imm12>}]     ; p=1, w=0
  //   strb<c> <Rt>, [<Rn>], #+/-<imm12>       ; p=1, w=1
  //   strb<c> <Rt>, [<Rn>, #+/-<imm12>]!      ; p=0, w=1
  //
  // cccc010pubw0nnnnttttiiiiiiiiiiii where cccc=Cond, tttt=Rt, nnnn=Rn,
  // iiiiiiiiiiii=imm12, b=1 if STRB, u=1 if +.
  constexpr bool IsLoad = false;
  const Type Ty = OpRt->getType();
  if (!(Ty == IceType_i32 || Ty == IceType_i8)) // TODO(kschimpf) Expand?
    return setNeedsTextFixup();
  const bool IsByte = isByteSizedType(Ty);
  // Check for rule violations.
  if ((getGPRReg(kRnShift, Address) == RegARM32::Encoded_Reg_pc))
    return setNeedsTextFixup();
  if (!isBitSet(P, Address) && isBitSet(W, Address))
    return setNeedsTextFixup();
  if (!IsByte && (getGPRReg(kRnShift, Address) == RegARM32::Encoded_Reg_sp) &&
      isBitSet(P, Address) && !isBitSet(U, Address) && isBitSet(W, Address) &&
      (mask(Address, kImm12Shift, kImmed12Bits) == 0x8 /* 000000000100 */))
    return setNeedsTextFixup();
  emitMemOp(Cond, kInstTypeMemImmediate, IsLoad, IsByte, Rt, Address);
}

void AssemblerARM32::orr(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  // ORR (register) - ARM Section A8.8.123, encoding A1:
  //   orr{s}<c> <Rd>, <Rn>, <Rm>
  //
  // cccc0001100snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, iiiii=shift, tt=ShiftKind,, and s=SetFlags.
  //
  // ORR (register) - ARM Section A8.8.123, encoding A1:
  //   orr{s}<c> <Rd>, <Rn>,  #<RotatedImm8>
  //
  // cccc0001100snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8.
  constexpr IValueT Orr = B3 | B2; // i.e. 1100
  emitType01(Orr, OpRd, OpRn, OpSrc1, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::pop(const Operand *OpRt, CondARM32::Cond Cond) {
  // POP - ARM section A8.8.132, encoding A2:
  //   pop<c> {Rt}
  //
  // cccc010010011101dddd000000000100 where dddd=Rt and cccc=Cond.
  IValueT Rt;
  if (decodeOperand(OpRt, Rt) != DecodedAsRegister)
    return setNeedsTextFixup();
  assert(Rt != RegARM32::Encoded_Reg_sp);
  // Same as load instruction.
  constexpr bool IsLoad = true;
  constexpr bool IsByte = false;
  IValueT Address = decodeImmRegOffset(RegARM32::Encoded_Reg_sp, kWordSize,
                                       OperandARM32Mem::PostIndex);
  emitMemOp(Cond, kInstTypeMemImmediate, IsLoad, IsByte, Rt, Address);
}

void AssemblerARM32::popList(const IValueT Registers, CondARM32::Cond Cond) {
  // POP - ARM section A8.*.131, encoding A1:
  //   pop<c> <registers>
  //
  // cccc100010111101rrrrrrrrrrrrrrrr where cccc=Cond and
  // rrrrrrrrrrrrrrrr=Registers (one bit for each GP register).
  constexpr bool IsLoad = true;
  emitMultiMemOp(Cond, IA_W, IsLoad, RegARM32::Encoded_Reg_sp, Registers);
}

void AssemblerARM32::push(const Operand *OpRt, CondARM32::Cond Cond) {
  // PUSH - ARM section A8.8.133, encoding A2:
  //   push<c> {Rt}
  //
  // cccc010100101101dddd000000000100 where dddd=Rt and cccc=Cond.
  IValueT Rt;
  if (decodeOperand(OpRt, Rt) != DecodedAsRegister)
    return setNeedsTextFixup();
  assert(Rt != RegARM32::Encoded_Reg_sp);
  // Same as store instruction.
  constexpr bool isLoad = false;
  constexpr bool isByte = false;
  IValueT Address = decodeImmRegOffset(RegARM32::Encoded_Reg_sp, -kWordSize,
                                       OperandARM32Mem::PreIndex);
  emitMemOp(Cond, kInstTypeMemImmediate, isLoad, isByte, Rt, Address);
}

void AssemblerARM32::pushList(const IValueT Registers, CondARM32::Cond Cond) {
  // PUSH - ARM section A8.8.133, encoding A1:
  //   push<c> <Registers>
  //
  // cccc100100101101rrrrrrrrrrrrrrrr where cccc=Cond and
  // rrrrrrrrrrrrrrrr=Registers (one bit for each GP register).
  constexpr bool IsLoad = false;
  emitMultiMemOp(Cond, DB_W, IsLoad, RegARM32::Encoded_Reg_sp, Registers);
}

void AssemblerARM32::mla(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpRm, const Operand *OpRa,
                         CondARM32::Cond Cond) {
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rn;
  if (decodeOperand(OpRn, Rn) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rm;
  if (decodeOperand(OpRm, Rm) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Ra;
  if (decodeOperand(OpRa, Ra) != DecodedAsRegister)
    return setNeedsTextFixup();
  // MLA - ARM section A8.8.114, encoding A1.
  //   mla{s}<c> <Rd>, <Rn>, <Rm>, <Ra>
  //
  // cccc0000001sddddaaaammmm1001nnnn where cccc=Cond, s=SetFlags, dddd=Rd,
  // aaaa=Ra, mmmm=Rm, and nnnn=Rn.
  if (Rd == RegARM32::Encoded_Reg_pc || Rn == RegARM32::Encoded_Reg_pc ||
      Rm == RegARM32::Encoded_Reg_pc || Ra == RegARM32::Encoded_Reg_pc)
    llvm::report_fatal_error("Mul instruction unpredictable on pc");
  constexpr IValueT MlaOpcode = B21;
  constexpr bool SetFlags = false;
  // Assembler registers rd, rn, rm, ra are encoded as rn, rm, rs, rd.
  emitMulOp(Cond, MlaOpcode, Ra, Rd, Rn, Rm, SetFlags);
}

void AssemblerARM32::mul(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rn;
  if (decodeOperand(OpRn, Rn) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rm;
  if (decodeOperand(OpSrc1, Rm) != DecodedAsRegister)
    return setNeedsTextFixup();
  // MUL - ARM section A8.8.114, encoding A1.
  //   mul{s}<c> <Rd>, <Rn>, <Rm>
  //
  // cccc0000000sdddd0000mmmm1001nnnn where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, and s=SetFlags.
  if (Rd == RegARM32::Encoded_Reg_pc || Rn == RegARM32::Encoded_Reg_pc ||
      Rm == RegARM32::Encoded_Reg_pc)
    llvm::report_fatal_error("Mul instruction unpredictable on pc");
  // Assembler registers rd, rn, rm are encoded as rn, rm, rs.
  constexpr IValueT MulOpcode = 0;
  emitMulOp(Cond, MulOpcode, RegARM32::Encoded_Reg_r0, Rd, Rn, Rm, SetFlags);
}

void AssemblerARM32::udiv(const Operand *OpRd, const Operand *OpRn,
                          const Operand *OpSrc1, CondARM32::Cond Cond) {
  // UDIV - ARM section A8.8.248, encoding A1.
  //   udiv<c> <Rd>, <Rn>, <Rm>
  //
  // cccc01110011dddd1111mmmm0001nnnn where cccc=Cond, dddd=Rd, nnnn=Rn, and
  // mmmm=Rm.
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rn;
  if (decodeOperand(OpRn, Rn) != DecodedAsRegister)
    return setNeedsTextFixup();
  IValueT Rm;
  if (decodeOperand(OpSrc1, Rm) != DecodedAsRegister)
    return setNeedsTextFixup();
  if (Rd == RegARM32::Encoded_Reg_pc || Rn == RegARM32::Encoded_Reg_pc ||
      Rm == RegARM32::Encoded_Reg_pc)
    llvm::report_fatal_error("Udiv instruction unpredictable on pc");
  // Assembler registers rd, rn, rm are encoded as rn, rm, rs.
  constexpr IValueT Opcode = B21;
  emitDivOp(Cond, Opcode, Rd, Rn, Rm);
}

void AssemblerARM32::sub(const Operand *OpRd, const Operand *OpRn,
                         const Operand *OpSrc1, bool SetFlags,
                         CondARM32::Cond Cond) {
  // SUB (register) - ARM section A8.8.223, encoding A1:
  //   sub{s}<c> <Rd>, <Rn>, <Rm>{, <shift>}
  // SUB (SP minus register): See ARM section 8.8.226, encoding A1:
  //   sub{s}<c> <Rd>, sp, <Rm>{, <Shift>}
  //
  // cccc0000010snnnnddddiiiiitt0mmmm where cccc=Cond, dddd=Rd, nnnn=Rn,
  // mmmm=Rm, iiiiii=shift, tt=ShiftKind, and s=SetFlags.
  //
  // Sub (Immediate) - ARM section A8.8.222, encoding A1:
  //    sub{s}<c> <Rd>, <Rn>, #<RotatedImm8>
  // Sub (Sp minus immediate) - ARM section A8.*.225, encoding A1:
  //    sub{s}<c> sp, <Rn>, #<RotatedImm8>
  //
  // cccc0010010snnnnddddiiiiiiiiiiii where cccc=Cond, dddd=Rd, nnnn=Rn,
  // s=SetFlags and iiiiiiiiiiii=Src1Value defining RotatedImm8
  constexpr IValueT Sub = B1; // 0010
  emitType01(Sub, OpRd, OpRn, OpSrc1, SetFlags, Cond, RdIsPcAndSetFlags);
}

void AssemblerARM32::tst(const Operand *OpRn, const Operand *OpSrc1,
                         CondARM32::Cond Cond) {
  // TST (register) - ARM section A8.8.241, encoding A1:
  //   tst<c> <Rn>, <Rm>(, <shift>}
  //
  // cccc00010001nnnn0000iiiiitt0mmmm where cccc=Cond, nnnn=Rn, mmmm=Rm,
  // iiiii=Shift, and tt=ShiftKind.
  //
  // TST (immediate) - ARM section A8.8.240, encoding A1:
  //   tst<c> <Rn>, #<RotatedImm8>
  //
  // cccc00110001nnnn0000iiiiiiiiiiii where cccc=Cond, nnnn=Rn, and
  // iiiiiiiiiiii defines RotatedImm8.
  constexpr IValueT Opcode = B3; // ie. 1000
  emitCompareOp(Opcode, OpRn, OpSrc1, Cond);
}

void AssemblerARM32::umull(const Operand *OpRdLo, const Operand *OpRdHi,
                           const Operand *OpRn, const Operand *OpRm,
                           CondARM32::Cond Cond) {
  // UMULL - ARM section A8.8.257, encoding A1:
  //   umull<c> <RdLo>, <RdHi>, <Rn>, <Rm>
  //
  // cccc0000100shhhhllllmmmm1001nnnn where hhhh=RdHi, llll=RdLo, nnnn=Rn,
  // mmmm=Rm, and s=SetFlags
  IValueT RdLo;
  IValueT RdHi;
  IValueT Rn;
  IValueT Rm;
  if (decodeOperand(OpRdLo, RdLo) != DecodedAsRegister ||
      decodeOperand(OpRdHi, RdHi) != DecodedAsRegister ||
      decodeOperand(OpRn, Rn) != DecodedAsRegister ||
      decodeOperand(OpRm, Rm) != DecodedAsRegister)
    return setNeedsTextFixup();
  if (RdHi == RegARM32::Encoded_Reg_pc || RdLo == RegARM32::Encoded_Reg_pc ||
      Rn == RegARM32::Encoded_Reg_pc || Rm == RegARM32::Encoded_Reg_pc ||
      RdHi == RdLo)
    llvm::report_fatal_error("Umull instruction unpredictable on pc");
  constexpr bool SetFlags = false;
  emitMulOp(Cond, B23, RdLo, RdHi, Rn, Rm, SetFlags);
}

void AssemblerARM32::uxt(const Operand *OpRd, const Operand *OpSrc0,
                         CondARM32::Cond Cond) {
  IValueT Rd;
  if (decodeOperand(OpRd, Rd) != DecodedAsRegister)
    return setNeedsTextFixup();
  // Note: For the moment, we assume no rotation is specified.
  RotationValue Rotation = kRotateNone;
  constexpr IValueT Rn = RegARM32::Encoded_Reg_pc;
  IValueT Rm;
  if (decodeOperand(OpSrc0, Rm) != DecodedAsRegister)
    return setNeedsTextFixup();
  switch (typeWidthInBytes(OpSrc0->getType())) {
  default:
    return setNeedsTextFixup();
  case 1: {
    // UXTB - ARM section A8.8.274, encoding A1:
    //   uxtb<c> <Rd>, <Rm>{, <rotate>}
    //
    // cccc011011101111ddddrr000111mmmm where cccc=Cond, dddd=Rd, mmmm=Rm, and
    // rr defined (RotationValue) rotate.
    constexpr IValueT Opcode = B26 | B25 | B23 | B22 | B21;
    emitUxt(Cond, Opcode, Rd, Rn, Rm, Rotation);
    return;
  }
  case 2: {
    // UXTH - ARM section A8.8.276, encoding A1:
    //   uxth<c> <Rd>< <Rm>{, <rotate>}
    //
    // cccc01101111nnnnddddrr000111mmmm where cccc=Cond, dddd=Rd, mmmm=Rm, and
    // rr defined (RotationValue) rotate.
    constexpr IValueT Opcode = B26 | B25 | B23 | B22 | B21 | B20;
    emitUxt(Cond, Opcode, Rd, Rn, Rm, Rotation);
    return;
  }
  }
}

} // end of namespace ARM32
} // end of namespace Ice
