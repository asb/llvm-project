//===-- WebAssemblyExplicitLocals.cpp - Make Locals Explicit --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file converts any remaining registers into WebAssembly locals.
///
/// After register stackification and register coloring, convert non-stackified
/// registers into locals, inserting explicit local.get and local.set
/// instructions.
///
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/WebAssemblyMCTargetDesc.h"
#include "Utils/WebAssemblyTypeUtilities.h"
#include "Utils/WebAssemblyUtilities.h"
#include "WebAssembly.h"
#include "WebAssemblyDebugValueManager.h"
#include "WebAssemblyMachineFunctionInfo.h"
#include "WebAssemblySubtarget.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
using namespace llvm;

#define DEBUG_TYPE "wasm-explicit-locals"

typedef std::pair<unsigned, wasm::ValType> LocalInfo;

namespace {
class WebAssemblyExplicitLocals final : public MachineFunctionPass {
  StringRef getPassName() const override {
    return "WebAssembly Explicit Locals";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addPreserved<MachineBlockFrequencyInfo>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;


public:
  static char ID; // Pass identification, replacement for typeid
  WebAssemblyExplicitLocals() : MachineFunctionPass(ID) {}
};
} // end anonymous namespace

char WebAssemblyExplicitLocals::ID = 0;
INITIALIZE_PASS(WebAssemblyExplicitLocals, DEBUG_TYPE,
                "Convert registers to WebAssembly locals", false, false)

FunctionPass *llvm::createWebAssemblyExplicitLocals() {
  return new WebAssemblyExplicitLocals();
}

static void checkFrameBase(WebAssemblyFunctionInfo &MFI, unsigned Local,
                           unsigned Reg) {
  // Mark a local for the frame base vreg.
  if (MFI.isFrameBaseVirtual() && Reg == MFI.getFrameBaseVreg()) {
    LLVM_DEBUG({
      dbgs() << "Allocating local " << Local << "for VReg "
             << Register::virtReg2Index(Reg) << '\n';
    });
    MFI.setFrameBaseLocal(Local);
  }
}

/// Return a local id number for the given register, assigning it a new one
/// if it doesn't yet have one.
static unsigned getLocalId(DenseMap<unsigned, LocalInfo> &Reg2Local,
                           WebAssemblyFunctionInfo &MFI, unsigned &CurLocal, wasm::ValType WVT,
                           unsigned Reg) {
  auto P = Reg2Local.insert(std::make_pair(Reg, std::make_pair(CurLocal, WVT)));
  if (P.second) {
    checkFrameBase(MFI, CurLocal, Reg);
    ++CurLocal;
  }
  return P.first->second.first;
}

static wasm::ValType getWVTForVReg(MachineFunction &,
                                   DenseMap<unsigned, LocalInfo> &, Register);

static wasm::ValType
getWVTForWasmRefDefMI(MachineFunction &MF,
                      DenseMap<unsigned, LocalInfo> &Reg2Local,
                      MachineInstr &MI) {
  switch (MI.getOpcode()) {
  default:
    report_fatal_error("Don't know how to determine WVT for this MI");
  case WebAssembly::LOCAL_TEE_WASMREF:
  case WebAssembly::TEE_WASMREF: {
    Register CopiedReg = MI.getOperand(2).getReg();
    auto RL = Reg2Local.find(CopiedReg);
    if (RL == Reg2Local.end())
      return getWVTForVReg(MF, Reg2Local, CopiedReg);
    return RL->second.second;
  }
  case WebAssembly::COPY_WASMREF: {
    Register CopiedReg = MI.getOperand(1).getReg();
    auto RL = Reg2Local.find(CopiedReg);
    if (RL == Reg2Local.end())
      return getWVTForVReg(MF, Reg2Local, CopiedReg);
    return RL->second.second;
  }
  case WebAssembly::GLOBAL_GET_WASMREF: {
    const GlobalValue *GV = MI.getOperand(1).getGlobal();
    return WebAssembly::retrieveValTypeForWasmRef(
        cast<TargetExtType>(GV->getValueType()));
  }
  case WebAssembly::REF_NULL_EXTERNREF:
    return wasm::ValType::EXTERNREF;
  case WebAssembly::REF_NULL_FUNCREF:
    return wasm::ValType::FUNCREF;
  }
}

static wasm::ValType getWVTForVReg(MachineFunction &MF,
                                   DenseMap<unsigned, LocalInfo> &Reg2Local,
                                   Register VReg) {
  // Return previously recorded WVT if we've seen the VReg before.
  auto RL = Reg2Local.find(VReg);
  if (RL != Reg2Local.end())
    return RL->second.second;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  wasm::ValType WVT = WebAssembly::regClassToValType(MRI.getRegClass(VReg));
  if (WVT != wasm::ValType::WASMREF)
    return WVT;

  // Determine the actual type for a WASMREF.
  if (MRI.def_empty(VReg))
    report_fatal_error("VReg unexpectedly has no defs");

  MachineRegisterInfo::def_instr_iterator I = MRI.def_instr_begin(VReg);
  WVT = getWVTForWasmRefDefMI(MF, Reg2Local, *I);
  // Check that any other uses have the same type.
  ++I;
  for (auto E = MRI.def_instr_end(); I != E; ++I) {
    if (WVT != getWVTForWasmRefDefMI(MF, Reg2Local, *I))
      report_fatal_error("WasmRef has defs with different type");
  }

  return WVT;
}

/// Get the appropriate drop opcode for the given register class.
static unsigned getDropOpcode(const TargetRegisterClass *RC) {
  if (RC == &WebAssembly::I32RegClass)
    return WebAssembly::DROP_I32;
  if (RC == &WebAssembly::I64RegClass)
    return WebAssembly::DROP_I64;
  if (RC == &WebAssembly::F32RegClass)
    return WebAssembly::DROP_F32;
  if (RC == &WebAssembly::F64RegClass)
    return WebAssembly::DROP_F64;
  if (RC == &WebAssembly::V128RegClass)
    return WebAssembly::DROP_V128;
  if (RC == &WebAssembly::WASMREFRegClass)
    return WebAssembly::DROP_WASMREF;
  llvm_unreachable("Unexpected register class");
}

/// Get the appropriate local.get opcode for the given register class.
static unsigned getLocalGetOpcode(const TargetRegisterClass *RC) {
  if (RC == &WebAssembly::I32RegClass)
    return WebAssembly::LOCAL_GET_I32;
  if (RC == &WebAssembly::I64RegClass)
    return WebAssembly::LOCAL_GET_I64;
  if (RC == &WebAssembly::F32RegClass)
    return WebAssembly::LOCAL_GET_F32;
  if (RC == &WebAssembly::F64RegClass)
    return WebAssembly::LOCAL_GET_F64;
  if (RC == &WebAssembly::V128RegClass)
    return WebAssembly::LOCAL_GET_V128;
  if (RC == &WebAssembly::WASMREFRegClass)
    return WebAssembly::LOCAL_GET_WASMREF;
  llvm_unreachable("Unexpected register class");
}

/// Get the appropriate local.set opcode for the given register class.
static unsigned getLocalSetOpcode(const TargetRegisterClass *RC) {
  if (RC == &WebAssembly::I32RegClass)
    return WebAssembly::LOCAL_SET_I32;
  if (RC == &WebAssembly::I64RegClass)
    return WebAssembly::LOCAL_SET_I64;
  if (RC == &WebAssembly::F32RegClass)
    return WebAssembly::LOCAL_SET_F32;
  if (RC == &WebAssembly::F64RegClass)
    return WebAssembly::LOCAL_SET_F64;
  if (RC == &WebAssembly::V128RegClass)
    return WebAssembly::LOCAL_SET_V128;
  if (RC == &WebAssembly::WASMREFRegClass)
    return WebAssembly::LOCAL_SET_WASMREF;
  llvm_unreachable("Unexpected register class");
}

/// Get the appropriate local.tee opcode for the given register class.
static unsigned getLocalTeeOpcode(const TargetRegisterClass *RC) {
  if (RC == &WebAssembly::I32RegClass)
    return WebAssembly::LOCAL_TEE_I32;
  if (RC == &WebAssembly::I64RegClass)
    return WebAssembly::LOCAL_TEE_I64;
  if (RC == &WebAssembly::F32RegClass)
    return WebAssembly::LOCAL_TEE_F32;
  if (RC == &WebAssembly::F64RegClass)
    return WebAssembly::LOCAL_TEE_F64;
  if (RC == &WebAssembly::V128RegClass)
    return WebAssembly::LOCAL_TEE_V128;
  if (RC == &WebAssembly::WASMREFRegClass)
    return WebAssembly::LOCAL_TEE_WASMREF;
  llvm_unreachable("Unexpected register class");
}

/// Given a MachineOperand of a stackified vreg, return the instruction at the
/// start of the expression tree.
static MachineInstr *findStartOfTree(MachineOperand &MO,
                                     MachineRegisterInfo &MRI,
                                     const WebAssemblyFunctionInfo &MFI) {
  Register Reg = MO.getReg();
  assert(MFI.isVRegStackified(Reg));
  MachineInstr *Def = MRI.getVRegDef(Reg);

  // If this instruction has any non-stackified defs, it is the start
  for (auto DefReg : Def->defs()) {
    if (!MFI.isVRegStackified(DefReg.getReg())) {
      return Def;
    }
  }

  // Find the first stackified use and proceed from there.
  for (MachineOperand &DefMO : Def->explicit_uses()) {
    if (!DefMO.isReg())
      continue;
    return findStartOfTree(DefMO, MRI, MFI);
  }

  // If there were no stackified uses, we've reached the start.
  return Def;
}

bool WebAssemblyExplicitLocals::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "********** Make Locals Explicit **********\n"
                       "********** Function: "
                    << MF.getName() << '\n');

  bool Changed = false;
  MachineRegisterInfo &MRI = MF.getRegInfo();
  WebAssemblyFunctionInfo &MFI = *MF.getInfo<WebAssemblyFunctionInfo>();
  const Function &F = MF.getFunction();
  const TargetMachine &TM = MF.getTarget();
  auto Sig = signatureFromFunctionType(*F.getParent(), F.getFunctionType(), &F, F, TM);
  const auto *TII = MF.getSubtarget<WebAssemblySubtarget>().getInstrInfo();

  // Map non-stackified virtual registers to their local ids.
  DenseMap<unsigned, LocalInfo> Reg2Local;

  // Handle ARGUMENTS first to ensure that they get the designated numbers.
  for (MachineBasicBlock::iterator I = MF.begin()->begin(),
                                   E = MF.begin()->end();
       I != E;) {
    MachineInstr &MI = *I++;
    if (!WebAssembly::isArgument(MI.getOpcode()))
      break;
    Register Reg = MI.getOperand(0).getReg();
    assert(!MFI.isVRegStackified(Reg));
    auto Local = static_cast<unsigned>(MI.getOperand(1).getImm());
    Reg2Local[Reg] = std::make_pair(Local, Sig->Params[Local]);
    checkFrameBase(MFI, Local, Reg);

    // Update debug value to point to the local before removing.
    WebAssemblyDebugValueManager(&MI).replaceWithLocal(Local);

    MI.eraseFromParent();
    Changed = true;
  }

  // Start assigning local numbers after the last parameter and after any
  // already-assigned locals.
  unsigned CurLocal = static_cast<unsigned>(MFI.getParams().size());
  CurLocal += static_cast<unsigned>(MFI.getLocals().size());

  // Precompute the set of registers that are unused, so that we can insert
  // drops to their defs.
  BitVector UseEmpty(MRI.getNumVirtRegs());
  for (unsigned I = 0, E = MRI.getNumVirtRegs(); I < E; ++I)
    UseEmpty[I] = MRI.use_empty(Register::index2VirtReg(I));

  // Visit each instruction in the function.
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : llvm::make_early_inc_range(MBB)) {
      assert(!WebAssembly::isArgument(MI.getOpcode()));

      if (MI.isDebugInstr() || MI.isLabel())
        continue;

      if (MI.getOpcode() == WebAssembly::IMPLICIT_DEF) {
        MI.eraseFromParent();
        Changed = true;
        continue;
      }

      // Replace tee instructions with local.tee. The difference is that tee
      // instructions have two defs, while local.tee instructions have one def
      // and an index of a local to write to.
      if (WebAssembly::isTee(MI.getOpcode())) {
        assert(MFI.isVRegStackified(MI.getOperand(0).getReg()));
        assert(!MFI.isVRegStackified(MI.getOperand(1).getReg()));
        Register OldReg = MI.getOperand(2).getReg();
        const TargetRegisterClass *RC = MRI.getRegClass(OldReg);

        // Stackify the input if it isn't stackified yet.
        if (!MFI.isVRegStackified(OldReg)) {
          unsigned LocalId =
              getLocalId(Reg2Local, MFI, CurLocal,
                         getWVTForVReg(MF, Reg2Local, OldReg), OldReg);
          Register NewReg = MRI.createVirtualRegister(RC);
          unsigned Opc = getLocalGetOpcode(RC);
          BuildMI(MBB, &MI, MI.getDebugLoc(), TII->get(Opc), NewReg)
              .addImm(LocalId);
          MI.getOperand(2).setReg(NewReg);
          MFI.stackifyVReg(MRI, NewReg);
        }

        // Replace the TEE with a LOCAL_TEE.
        Register OldReg2 = MI.getOperand(1).getReg();
        unsigned LocalId =
            getLocalId(Reg2Local, MFI, CurLocal,
                       getWVTForVReg(MF, Reg2Local, OldReg2), OldReg2);
        unsigned Opc = getLocalTeeOpcode(RC);
        BuildMI(MBB, &MI, MI.getDebugLoc(), TII->get(Opc),
                MI.getOperand(0).getReg())
            .addImm(LocalId)
            .addReg(MI.getOperand(2).getReg());

        WebAssemblyDebugValueManager(&MI).replaceWithLocal(LocalId);

        MI.eraseFromParent();
        Changed = true;
        continue;
      }

      // Insert local.sets for any defs that aren't stackified yet.
      for (auto &Def : MI.defs()) {
        Register OldReg = Def.getReg();
        if (!MFI.isVRegStackified(OldReg)) {
          const TargetRegisterClass *RC = MRI.getRegClass(OldReg);
          Register NewReg = MRI.createVirtualRegister(RC);
          auto InsertPt = std::next(MI.getIterator());
          if (UseEmpty[Register::virtReg2Index(OldReg)]) {
            unsigned Opc = getDropOpcode(RC);
            MachineInstr *Drop =
                BuildMI(MBB, InsertPt, MI.getDebugLoc(), TII->get(Opc))
                    .addReg(NewReg);
            // After the drop instruction, this reg operand will not be used
            Drop->getOperand(0).setIsKill();
            if (MFI.isFrameBaseVirtual() && OldReg == MFI.getFrameBaseVreg())
              MFI.clearFrameBaseVreg();
          } else {
            unsigned LocalId =
                getLocalId(Reg2Local, MFI, CurLocal,
                           getWVTForVReg(MF, Reg2Local, OldReg), OldReg);
            unsigned Opc = getLocalSetOpcode(RC);

            WebAssemblyDebugValueManager(&MI).replaceWithLocal(LocalId);

            BuildMI(MBB, InsertPt, MI.getDebugLoc(), TII->get(Opc))
                .addImm(LocalId)
                .addReg(NewReg);
          }
          // This register operand of the original instruction is now being used
          // by the inserted drop or local.set instruction, so make it not dead
          // yet.
          Def.setReg(NewReg);
          Def.setIsDead(false);
          MFI.stackifyVReg(MRI, NewReg);
          Changed = true;
        }
      }

      // Insert local.gets for any uses that aren't stackified yet.
      MachineInstr *InsertPt = &MI;
      for (MachineOperand &MO : reverse(MI.explicit_uses())) {
        if (!MO.isReg())
          continue;

        Register OldReg = MO.getReg();

        if (MO.isUndef() &&
            MRI.getRegClass(OldReg) == &WebAssembly::WASMREFRegClass)
          report_fatal_error("Can't create local.get for an undef wasmref");

        // Inline asm may have a def in the middle of the operands. Our contract
        // with inline asm register operands is to provide local indices as
        // immediates.
        if (MO.isDef()) {
          assert(MI.isInlineAsm());
          unsigned LocalId =
              getLocalId(Reg2Local, MFI, CurLocal,
                         getWVTForVReg(MF, Reg2Local, OldReg), OldReg);
          // If this register operand is tied to another operand, we can't
          // change it to an immediate. Untie it first.
          MI.untieRegOperand(MO.getOperandNo());
          MO.ChangeToImmediate(LocalId);
          continue;
        }

        // If we see a stackified register, prepare to insert subsequent
        // local.gets before the start of its tree.
        if (MFI.isVRegStackified(OldReg)) {
          InsertPt = findStartOfTree(MO, MRI, MFI);
          continue;
        }

        // Our contract with inline asm register operands is to provide local
        // indices as immediates.
        if (MI.isInlineAsm()) {
          unsigned LocalId =
              getLocalId(Reg2Local, MFI, CurLocal,
                         getWVTForVReg(MF, Reg2Local, OldReg), OldReg);
          // Untie it first if this reg operand is tied to another operand.
          MI.untieRegOperand(MO.getOperandNo());
          MO.ChangeToImmediate(LocalId);
          continue;
        }

        // Insert a local.get.
        unsigned LocalId =
            getLocalId(Reg2Local, MFI, CurLocal,
                       getWVTForVReg(MF, Reg2Local, OldReg), OldReg);
        const TargetRegisterClass *RC = MRI.getRegClass(OldReg);
        Register NewReg = MRI.createVirtualRegister(RC);
        unsigned Opc = getLocalGetOpcode(RC);
        // Use a InsertPt as our DebugLoc, since MI may be discontinuous from
        // the where this local is being inserted, causing non-linear stepping
        // in the debugger or function entry points where variables aren't live
        // yet. Alternative is previous instruction, but that is strictly worse
        // since it can point at the previous statement.
        // See crbug.com/1251909, crbug.com/1249745
        InsertPt = BuildMI(MBB, InsertPt, InsertPt->getDebugLoc(),
                           TII->get(Opc), NewReg).addImm(LocalId);
        MO.setReg(NewReg);
        MFI.stackifyVReg(MRI, NewReg);
        Changed = true;
      }

      // Coalesce and eliminate COPY instructions.
      if (WebAssembly::isCopy(MI.getOpcode())) {
        MRI.replaceRegWith(MI.getOperand(1).getReg(),
                           MI.getOperand(0).getReg());
        MI.eraseFromParent();
        Changed = true;
      }
    }
  }

  // Define the locals.
  // TODO: Sort the locals for better compression.
  MFI.setNumLocals(CurLocal - MFI.getParams().size());
  for (unsigned I = 0, E = MRI.getNumVirtRegs(); I < E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    auto RL = Reg2Local.find(Reg);
    if (RL == Reg2Local.end() || RL->second.first < MFI.getParams().size())
      continue;

    wasm::ValType WVT = RL->second.second;
    if (WVT == wasm::ValType::WASMREF)
      report_fatal_error(
          "WASMREF WVTs should have been eliminated by this point");
    MFI.setLocal(RL->second.first - MFI.getParams().size(), WVT);
    Changed = true;
  }

#ifndef NDEBUG
  // Assert that all registers have been stackified at this point.
  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      if (MI.isDebugInstr() || MI.isLabel())
        continue;
      for (const MachineOperand &MO : MI.explicit_operands()) {
        assert(
            (!MO.isReg() || MRI.use_empty(MO.getReg()) ||
             MFI.isVRegStackified(MO.getReg())) &&
            "WebAssemblyExplicitLocals failed to stackify a register operand");
      }
    }
  }
#endif

  return Changed;
}
