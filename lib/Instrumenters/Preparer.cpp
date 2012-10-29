// vim: sw=2

// Author: Jingyue

#define DEBUG_TYPE "dyn-aa"

#include <string>

#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Constants.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetData.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"

#include "rcs/typedefs.h"
#include "rcs/IDAssigner.h"

#include "dyn-aa/Utils.h"

using namespace llvm;
using namespace std;
using namespace rcs;

namespace dyn_aa {
struct Preparer: public ModulePass {
  static char ID;

  Preparer();
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual bool runOnModule(Module &M);

 private:
  static unsigned RoundUpToPowerOfTwo(unsigned Value);
  void replaceUndefsWithNull(Module &M);
  // use-def chains sometimes form a cycle.
  // Do not visit a User twice by using Replaced.
  void replaceUndefsWithNull(User *I, ValueSet &Replaced);
  void allocateExtraBytes(Module &M);
  void expandAllocas(Function *F);
  void expandAlloca(AllocaInst *AI);
  void fillInAllocationSize(Module &M);
  void fillInAllocationSize(CallSite CS);
};
}

using namespace dyn_aa;

char Preparer::ID = 0;

static RegisterPass<Preparer> X(
    "prepare",
    "Preparing transformations for both online and offline mode",
    false, false);

void Preparer::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetData>();
}

Preparer::Preparer(): ModulePass(ID) {}

bool Preparer::runOnModule(Module &M) {
  replaceUndefsWithNull(M);
  allocateExtraBytes(M);
  fillInAllocationSize(M);
  return true;
}

void Preparer::replaceUndefsWithNull(Module &M) {
  ValueSet Replaced;
  for (Module::global_iterator GI = M.global_begin(); GI != M.global_end();
       ++GI) {
    if (GI->hasInitializer()) {
      replaceUndefsWithNull(GI->getInitializer(), Replaced);
    }
  }
  for (Module::iterator F = M.begin(); F != M.end(); ++F) {
    for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
      for (BasicBlock::iterator Ins = BB->begin(); Ins != BB->end(); ++Ins) {
        replaceUndefsWithNull(Ins, Replaced);
      }
    }
  }
}

void Preparer::replaceUndefsWithNull(User *I, ValueSet &Replaced) {
  if (Replaced.count(I))
    return;
  Replaced.insert(I);
  for (User::op_iterator OI = I->op_begin(); OI != I->op_end(); ++OI) {
    Value *V = OI->get();
    if (isa<UndefValue>(V) && V->getType()->isPointerTy()) {
      OI->set(ConstantPointerNull::get(cast<PointerType>(V->getType())));
    }
    if (User *I2 = dyn_cast<User>(V)) {
      replaceUndefsWithNull(I2, Replaced);
    }
  }
}

void Preparer::allocateExtraBytes(Module &M) {
  for (Module::iterator F = M.begin(); F != M.end(); ++F) {
    expandAllocas(F);
  }
}

void Preparer::expandAllocas(Function *F) {
  for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
    for (BasicBlock::iterator Ins = BB->begin(); Ins != BB->end(); ) {
      // <Ins> may be removed in the body. Save its next instruction.
      BasicBlock::iterator NextIns = Ins; ++NextIns;
      if (AllocaInst *AI = dyn_cast<AllocaInst>(Ins))
        expandAlloca(AI);
      Ins = NextIns;
    }
  }
}

void Preparer::expandAlloca(AllocaInst *AI) {
  // Skip dynaa.slots which is added by AliasCheckerInstrumenter.
  if (AI->getName().startswith(DynAAUtils::SlotsName))
    return;

  if (AI->isArrayAllocation()) {
    // e.g. %32 = alloca i8, i64 %conv164
    Value *Size = AI->getArraySize();
    Value *ExpandedSize = BinaryOperator::Create(
        Instruction::Add,
        Size,
        ConstantInt::get(cast<IntegerType>(Size->getType()), 1),
        "expanded.size",
        AI);
    AI->setOperand(0, ExpandedSize);
    return;
  }

  Type *AllocatedType = AI->getAllocatedType();
  if (ArrayType *ArrType = dyn_cast<ArrayType>(AllocatedType)) {
    ArrayType *NewArrType = ArrayType::get(ArrType->getElementType(),
                                           ArrType->getNumElements() + 1);
    AllocaInst *NewAI = new AllocaInst(NewArrType, AI->getName(), AI);
    BitCastInst *CastNewAI = new BitCastInst(NewAI,
                                             AI->getType(),
                                             AI->getName(),
                                             AI);
    AI->replaceAllUsesWith(CastNewAI);
    AI->eraseFromParent();
    return;
  }

  assert(AllocatedType->isSized());
  TargetData &TD = getAnalysis<TargetData>();
  unsigned Align = RoundUpToPowerOfTwo(TD.getTypeStoreSize(AllocatedType) + 1);
  assert(AI->getAlignment() > 0);
  Align = max(Align, AI->getAlignment());
  AI->setAlignment(Align);
}

unsigned Preparer::RoundUpToPowerOfTwo(unsigned Value) {
  // TODO: should be able to be optimized using bitwise operations
  unsigned Result;
  for (Result = 1; Result < Value; Result *= 2);
  return Result;
}

void Preparer::fillInAllocationSize(Module &M) {
  Function *MemAllocHook = M.getFunction(DynAAUtils::MemAllocHookName);
  // Skip this process if there's no HookMemAlloc.
  if (!MemAllocHook)
    return;

  for (Module::iterator F = M.begin(); F != M.end(); ++F) {
    for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
      for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
        CallSite CS(I);
        if (CS && CS.getCalledFunction() == MemAllocHook) {
          fillInAllocationSize(CS);
        }
      }
    }
  }
}

// CallSite is light-weight, and passed by value.
void Preparer::fillInAllocationSize(CallSite CS) {
  // HookMemAlloc(ValueID, Base, Size = undef)
  assert(CS.arg_size() == 3);
  if (isa<UndefValue>(CS.getArgument(2))) {
    Value *Base = CS.getArgument(1);
    while (BitCastInst *BCI = dyn_cast<BitCastInst>(Base)) {
      Base = BCI->getOperand(0);
    }
    // For now, MemoryInstrumenter will only use undef for the allocation size
    // for AllocaInsts.
    if (AllocaInst *AI = dyn_cast<AllocaInst>(Base)) {
      TargetData &TD = getAnalysis<TargetData>();
      Value *Size = ConstantInt::get(
          TD.getIntPtrType(AI->getContext()),
          TD.getTypeStoreSize(AI->getAllocatedType()));
      if (AI->isArrayAllocation()) {
        // e.g. %32 = alloca i8, i64 %conv164
        Size = BinaryOperator::Create(Instruction::Mul,
                                      Size,
                                      AI->getArraySize(),
                                      "",
                                      AI);
      }
      CS.setArgument(2, Size);
    } else {
      assert(false);
    }
  }
}
