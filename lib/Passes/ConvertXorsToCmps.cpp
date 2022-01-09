/*
 * Copyright (c) 2019-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include <anvill/Passes/ConvertXorsToCmps.h>

#include <anvill/ABI.h>
#include <glog/logging.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include <optional>
#include <tuple>
#include <vector>

#include "Utils.h"

namespace anvill {
namespace {

// If the operator (op) is between an ICmpInst and a ConstantInt, return a
// tuple representing the ICmpInst and ConstantInt with tuple[0] holding the
// ICmpInst. Otherwise return `nullopt`.
static std::optional<std::tuple<llvm::ICmpInst *, llvm::ConstantInt *>>
getComparisonOperands(llvm::BinaryOperator *op) {

  auto lhs_c = llvm::dyn_cast<llvm::ConstantInt>(op->getOperand(0));
  auto lhs_cmp = llvm::dyn_cast<llvm::ICmpInst>(op->getOperand(0));

  auto rhs_c = llvm::dyn_cast<llvm::ConstantInt>(op->getOperand(1));
  auto rhs_cmp = llvm::dyn_cast<llvm::ICmpInst>(op->getOperand(1));

  // right side: predicate, left side; constant int;
  if (rhs_cmp && lhs_c) {
    return {{rhs_cmp, lhs_c}};
  }

  // right side: constant int, left side: cmp
  if (rhs_c && lhs_cmp) {
    return {{lhs_cmp, rhs_c}};
  }

  return std::nullopt;
}


static llvm::Value *negateCmpPredicate(llvm::ICmpInst *cmp) {
  auto pred = cmp->getPredicate();
  llvm::IRBuilder<> ir(cmp);
  llvm::ICmpInst::Predicate new_pred = llvm::CmpInst::getInversePredicate(pred);

  // Create a new compare with negated predicate.
  return ir.CreateICmp(new_pred, cmp->getOperand(0), cmp->getOperand(1));
}

}  // namespace


llvm::PreservedAnalyses
ConvertXorsToCmps::run(llvm::Function &func, llvm::FunctionAnalysisManager &AM) {
  std::vector<llvm::BinaryOperator *> xors;

  for (auto &inst : llvm::instructions(func)) {

    // check for binary op
    if (auto binop = llvm::dyn_cast<llvm::BinaryOperator>(&inst)) {

      // binary op is a xor
      if (binop->getOpcode() == llvm::Instruction::Xor) {

        // Get comparison operands of the xor. The caller ensures that one is a
        // compare and the other is a constant integer.
        auto cmp_ops = getComparisonOperands(binop);
        if (cmp_ops.has_value()) {
          auto [_, cnst_int] = cmp_ops.value();

          // ensure that the constant int is 'true', or an i1 with the value 1
          // this (currently) the only supported value
          if (cnst_int->getType()->getBitWidth() == 1 &&
              cnst_int->isAllOnesValue()) {
            xors.emplace_back(binop);
          }
        }
      }
    }
  }

  auto changed = false;
  int replaced_items = 0;

  std::vector<llvm::BranchInst *> brs_to_invert;
  std::vector<llvm::SelectInst *> selects_to_invert;

  for (auto xori : xors) {

    // find predicate from xor's operands
    auto cmp_ops = getComparisonOperands(xori);
    if (!cmp_ops.has_value()) {
      continue;
    }
    auto [cmp, _] = cmp_ops.value();

    bool invertible_xor = true;

    // so far, we have matched the following pattern:
    //
    //   %c = icmp PREDICATE v1, v2
    //   %x = xor i1 %c, true
    //
    // We want to to fold this cmp/xor pair into a cmp with an inverse
    // predicate, like so:
    //
    //   %c = icmp !PREDICATE v1, v2
    //   %x = %c
    //
    // BUT! Depending on how %c is used we may or may not be able to do that.
    //
    // We need to know if the result of the comparison (%c) is used elsewhere,
    // and how.
    //
    // We *can* still invert the cmp/xor pair if:
    // * All uses of this `cmp` are either a SelectInst or a BranchInst
    //    then we will invert every select and branch condition.
    //    this gets rid of the xor, and preserves program logic
    //
    // Examples:
    //  %si = select i1 %c, true_val, false_val
    //  br i1 %c, label %left, label %right
    //
    // We *cannot* invert the cmp/xor pair if:
    // * One or more uses of this `cmp` is *NOT* a SelectInst or a BranchInst
    //    The original value could be stored or used in some arithmetic op
    //    and we cannot freely invert the comparison, because it would change
    //    the logic of the program.
    //
    //    Most common example? A zext, like the following:
    //     %z = zext i1 %c to i64

    for (auto &U : cmp->uses()) {
      llvm::Instruction *inst = llvm::dyn_cast<llvm::Instruction>(U.getUser());

      // use is not the existing xor
      if (!inst || inst == xori) {
        continue;
      }

      brs_to_invert.clear();
      selects_to_invert.clear();

      llvm::BranchInst *br = llvm::dyn_cast<llvm::BranchInst>(inst);

      // A user of this compare is a BranchInstruction
      if (br) {
        brs_to_invert.emplace_back(br);
        continue;
      }

      llvm::SelectInst *si = llvm::dyn_cast<llvm::SelectInst>(inst);

      // A user of this compare is a SelectInst, and the compare is the
      // condition and not an operand.
      if (si && llvm::dyn_cast<llvm::ICmpInst>(si->getCondition()) == cmp) {
        selects_to_invert.emplace_back(si);
        continue;
      }

      invertible_xor = false;
      break;
    }

    // not inverting this branch
    if (!invertible_xor) {
      continue;
    }

    // negate predicate
    auto neg_cmp = negateCmpPredicate(cmp);
    if (neg_cmp) {
      CopyMetadataTo(xori, neg_cmp);
      replaced_items += 1;

      // invert all branches
      for (auto B : brs_to_invert) {
        B->swapSuccessors();
      }

      // invert all selects
      for (auto SI : selects_to_invert) {
        SI->swapValues();
      }

      // replace uses of predicate with negated predicate
      cmp->replaceAllUsesWith(neg_cmp);

      // delete original predicate
      cmp->eraseFromParent();

      // replace uses of xor with negated predicate
      xori->replaceAllUsesWith(neg_cmp);

      // delte xor
      xori->eraseFromParent();
      changed = true;
    }
  }

  return ConvertBoolToPreserved(changed);
}


llvm::StringRef ConvertXorsToCmps::name(void) {
  return "ConvertXorsToCmps";
}

// Convert operations in the form of:
//      (left OP right) ^ 1
// into:
//      (left !OP right)
// this makes the output more natural for humans and computers to reason about
// This problem comes up a fair bit due to how some instruction semantics
// compute carry/parity/etc bits.
void AddConvertXorsToCmps(llvm::FunctionPassManager &fpm) {
  fpm.addPass(ConvertXorsToCmps());
}

}  // namespace anvill