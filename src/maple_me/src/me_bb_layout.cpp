/*
 * Copyright (c) [2019] Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * OpenArkCompiler is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
 */
#include "me_bb_layout.h"
#include "me_cfg.h"
#include "bb.h"
#include "me_irmap.h"
#include "me_option.h"

// This BB layout strategy strictly obeys source ordering when inside try blocks.
// This Optimization will reorder the bb layout. it start from the first bb of func.
// All bbs will be put into layoutBBs and it gives the determined layout order.
// The entry of the optimization is MeDoBBLayout::Run. It starts from the first bb.
// 1. If curBB is condtion goto or goto kind, do OptimizeBranchTarget for bb.
// 2. Find curBB's next bb nextBB, and based on nextBB do the following:
// 3. (1) For fallthru/catch/finally, fix curBB's fallthru
//    (2) For condtion goto curBB:
//        i) If the target bb can be moved, then put it as currBB's next
//        and retarget curBB to it's fallthru bb. add targetBB as next.
//        ii) If curBB's fallthru is not its next bb add fallthru as its next if
//            fallthru can be moved else create a new fallthru contains a goto to
//            the original fallthru
//    (3) For goto curBB see if goto target can be placed as next.
// 5. do step 3 for nextBB until all bbs are laid out
namespace maple {
static void CreateGoto(BB &bb, MeFunction &func, BB &fallthru) {
  LabelIdx label = func.GetOrCreateBBLabel(fallthru);
  if (func.GetIRMap() != nullptr) {
    GotoNode stmt(OP_goto);
    auto *newGoto = func.GetIRMap()->New<GotoMeStmt>(&stmt);
    newGoto->SetOffset(label);
    bb.AddMeStmtLast(newGoto);
  } else {
    auto *newGoto = func.GetMirFunc()->GetCodeMempool()->New<GotoNode>(OP_goto);
    newGoto->SetOffset(label);
    bb.AddStmtNode(newGoto);
  }
  bb.SetKind(kBBGoto);
}

// return true if bb is empty and its kind is fallthru.
bool BBLayout::BBEmptyAndFallthru(const BB &bb) {
  if (bb.GetAttributes(kBBAttrIsTryEnd)) {
    return false;
  }
  if (bb.GetKind() == kBBFallthru) {
    if (func.GetIRMap() != nullptr) {
      return bb.IsMeStmtEmpty();
    }
    return bb.IsEmpty();
  }
  return false;
}

// Return true if bb only has conditonal branch stmt except comment
bool BBLayout::BBContainsOnlyCondGoto(const BB &bb) const {
  if (bb.GetKind() != kBBCondGoto || bb.GetAttributes(kBBAttrIsTryEnd)) {
    return false;
  }

  if (func.GetIRMap() != nullptr) {
    auto &meStmts = bb.GetMeStmts();
    if (meStmts.empty()) {
      return false;
    }
    for (auto itMeStmt = meStmts.begin(); itMeStmt != meStmts.rbegin().base(); ++itMeStmt) {
      if (!itMeStmt->IsCondBr() && itMeStmt->GetOp() != OP_comment) {
        return false;
      }
    }
    return meStmts.back().IsCondBr();
  }
  auto &stmtNodes = bb.GetStmtNodes();
  if (stmtNodes.empty()) {
    return false;
  }
  for (auto itStmt = stmtNodes.begin(); itStmt != stmtNodes.rbegin().base(); ++itStmt) {
    if (!itStmt->IsCondBr() && itStmt->GetOpCode() != OP_comment) {
      return false;
    }
  }
  return bb.GetStmtNodes().back().IsCondBr();
}

// Return the opposite opcode for condition/compare opcode.
static Opcode GetOppositeOp(Opcode opcInput) {
  Opcode opc = OP_undef;
  switch (opcInput) {
    case OP_brtrue:
      opc = OP_brfalse;
      break;
    case OP_brfalse:
      opc = OP_brtrue;
      break;
    case OP_ne:
      opc = OP_eq;
      break;
    case OP_eq:
      opc = OP_ne;
      break;
    case OP_gt:
      opc = OP_le;
      break;
    case OP_le:
      opc = OP_gt;
      break;
    case OP_lt:
      opc = OP_ge;
      break;
    case OP_ge:
      opc = OP_lt;
      break;
    default:
      break;
  }
  return opc;
}

bool BBLayout::BBContainsOnlyGoto(const BB &bb) const {
  if (bb.GetKind() != kBBGoto || bb.GetAttributes(kBBAttrIsTryEnd)) {
    return false;
  }

  if (func.GetIRMap() != nullptr) {
    auto &meStmts = bb.GetMeStmts();
    if (meStmts.empty()) {
      return false;
    }
    for (auto itMeStmt = meStmts.begin(); itMeStmt != meStmts.rbegin().base(); ++itMeStmt) {
      if (itMeStmt->GetOp() != OP_goto && itMeStmt->GetOp() != OP_comment) {
        return false;
      }
    }
    return meStmts.back().GetOp() == OP_goto;
  }
  auto &stmtNodes = bb.GetStmtNodes();
  if (stmtNodes.empty()) {
    return false;
  }
  for (auto itStmt = stmtNodes.begin(); itStmt != stmtNodes.rbegin().base(); ++itStmt) {
    if (itStmt->GetOpCode() != OP_goto && itStmt->GetOpCode() != OP_comment) {
      return false;
    }
  }
  return bb.GetStmtNodes().back().GetOpCode() == OP_goto;
}

// Return true if all the following are satisfied:
// 1.fromBB only has one predecessor
// 2.fromBB has not been laid out.
// 3.fromBB has only one succor when fromBB is artifical or fromBB and
//   toafter_bb are both not in try block.
// The other case is fromBB has one predecessor and one successor and
// contains only goto stmt.
bool BBLayout::BBCanBeMoved(const BB &fromBB, const BB &toAfterBB) const {
  if (fromBB.GetPred().size() > 1) {
    return false;
  }
  if (laidOut[fromBB.GetBBId()]) {
    return false;
  }
  if (fromBB.GetAttributes(kBBAttrArtificial) ||
      (!fromBB.GetAttributes(kBBAttrIsTry) && !toAfterBB.GetAttributes(kBBAttrIsTry))) {
    return fromBB.GetSucc().size() == 1;
  }
  return BBContainsOnlyGoto(fromBB);
}

// Return true if bb1 and bb2 has the branch conditon.such as
// bb1 : brfalse (a > 3)  bb2: brfalse (a > 3)/ brtrue (a <= 3)
bool BBLayout::HasSameBranchCond(BB &bb1, BB &bb2) const {
  if (func.GetIRMap() == nullptr) {
    return false;
  }
  auto &meStmt1 = static_cast<CondGotoMeStmt&>(bb1.GetMeStmts().back());
  auto &meStmt2 = static_cast<CondGotoMeStmt&>(bb2.GetMeStmts().back());
  MeExpr *expr1 = meStmt1.GetOpnd();
  MeExpr *expr2 = meStmt2.GetOpnd();
  // Compare the opcode:  brtrue/brfalse
  if (!(meStmt1.GetOp() == meStmt2.GetOp() && expr1->GetOp() == expr2->GetOp()) &&
      !(meStmt1.GetOp() == GetOppositeOp(meStmt2.GetOp()) && expr1->GetOp() == GetOppositeOp(expr2->GetOp()))) {
    return false;
  }
  if (!(expr1->GetMeOp() == expr2->GetMeOp() && expr1->GetMeOp() == kMeOpOp)) {
    return false;
  }
  auto *opMeExpr1 = static_cast<OpMeExpr*>(expr1);
  auto *opMeExpr2 = static_cast<OpMeExpr*>(expr2);
  // Compare the two operands to make sure they are both equal.
  if (opMeExpr1->GetOpnd(0) != opMeExpr2->GetOpnd(0)) {
    return false;
  }
  // If one side is const, assume it is always the rhs.
  if ((opMeExpr1->GetOpnd(1) != opMeExpr2->GetOpnd(1)) &&
      !(opMeExpr1->GetOpnd(1)->IsZero() && opMeExpr2->GetOpnd(1)->IsZero())) {
    return false;
  }
  return true;
}

// (1) bb's last statement is a conditional or unconditional branch; if the branch
// target is a BB with only a single goto statement, optimize the branch target
// to the eventual target
// (2) bb's last statement is a conditonal branch, if the branch target is a BB with a single
// condtioal branch statement and has the same condtion as bb's last statement, optimize the
// branch target to the eventual target.
void BBLayout::OptimizeBranchTarget(BB &bb) {
  if (func.GetIRMap() != nullptr) {
    auto &meStmts = bb.GetMeStmts();
    if (meStmts.empty()) {
      return;
    }
    if (meStmts.back().GetOp() != OP_goto && !meStmts.back().IsCondBr()) {
      return;
    }
  } else {
    auto &stmtNodes = bb.GetStmtNodes();
    if (stmtNodes.empty()) {
      return;
    }
    if (stmtNodes.back().GetOpCode() != OP_goto && !stmtNodes.back().IsCondBr()) {
      return;
    }
  }
  do {
    ASSERT(!bb.GetSucc().empty(), "container check");
    BB *brTargetBB = bb.GetKind() == kBBCondGoto ? bb.GetSucc(1) : bb.GetSucc(0);
    if (brTargetBB->GetAttributes(kBBAttrWontExit)) {
      return;
    }
    if (!BBContainsOnlyGoto(*brTargetBB) && !BBEmptyAndFallthru(*brTargetBB) &&
        !(bb.GetKind() == kBBCondGoto && brTargetBB->GetKind() == kBBCondGoto && &bb != brTargetBB &&
          BBContainsOnlyCondGoto(*brTargetBB) && HasSameBranchCond(bb, *brTargetBB))) {
      return;
    }
    // optimize stmt
    BB *newTargetBB = brTargetBB->GetSucc().front();
    if (brTargetBB->GetKind() == kBBCondGoto) {
      newTargetBB = brTargetBB->GetSucc(1);
    }
    LabelIdx newTargetLabel = func.GetOrCreateBBLabel(*newTargetBB);
    if (func.GetIRMap() != nullptr) {
      auto &lastStmt = bb.GetMeStmts().back();
      if (lastStmt.GetOp() == OP_goto) {
        auto &gotoMeStmt = static_cast<GotoMeStmt&>(lastStmt);
        ASSERT(brTargetBB->GetBBLabel() == gotoMeStmt.GetOffset(), "OptimizeBranchTarget: wrong branch target BB");
        gotoMeStmt.SetOffset(newTargetLabel);
      } else {
        auto &gotoMeStmt = static_cast<CondGotoMeStmt&>(lastStmt);
        ASSERT(brTargetBB->GetBBLabel() == gotoMeStmt.GetOffset(), "OptimizeBranchTarget: wrong branch target BB");
        gotoMeStmt.SetOffset(newTargetLabel);
      }
    } else {
      StmtNode &lastStmt = bb.GetStmtNodes().back();
      if (lastStmt.GetOpCode() == OP_goto) {
        auto &gotoNode = static_cast<GotoNode&>(lastStmt);
        ASSERT(brTargetBB->GetBBLabel() == gotoNode.GetOffset(), "OptimizeBranchTarget: wrong branch target BB");
        gotoNode.SetOffset(newTargetLabel);
      } else {
        auto &gotoNode = static_cast<CondGotoNode&>(lastStmt);
        ASSERT(brTargetBB->GetBBLabel() == gotoNode.GetOffset(), "OptimizeBranchTarget: wrong branch target BB");
        gotoNode.SetOffset(newTargetLabel);
      }
    }
    // update CFG
    bb.ReplaceSucc(brTargetBB, newTargetBB);
    bb.RemoveBBFromVector(brTargetBB->GetPred());
    if (brTargetBB->GetPred().empty()) {
      laidOut[brTargetBB->GetBBId()] = true;
      RemoveUnreachable(*brTargetBB);
    }
  } while (true);
}

void BBLayout::AddBB(BB &bb) {
  CHECK_FATAL(bb.GetBBId() < laidOut.size(), "index out of range in BBLayout::AddBB");
  ASSERT(!laidOut[bb.GetBBId()], "AddBB: bb already laid out");
  layoutBBs.push_back(&bb);
  laidOut[bb.GetBBId()] = true;
  if (enabledDebug) {
    LogInfo::MapleLogger() << "bb id " << bb.GetBBId() << " kind is " << bb.StrAttribute();
  }
  bool isTry = false;
  if (func.GetIRMap() != nullptr) {
    isTry = !bb.GetMeStmts().empty() && bb.GetMeStmts().front().GetOp() == OP_try;
  } else {
    isTry = !bb.GetStmtNodes().empty() && bb.GetStmtNodes().front().GetOpCode() == OP_try;
  }
  if (isTry) {
    ASSERT(!tryOutstanding, "BBLayout::AddBB: cannot lay out another try without ending the last one");
    tryOutstanding = true;
    if (enabledDebug) {
      LogInfo::MapleLogger() << " try";
    }
  }
  if (bb.GetAttributes(kBBAttrIsTryEnd) && func.GetMIRModule().IsJavaModule()) {
    tryOutstanding = false;
    if (enabledDebug) {
      LogInfo::MapleLogger() << " endtry";
    }
  }
  if (enabledDebug) {
    LogInfo::MapleLogger() << '\n';
  }
}

BB *BBLayout::GetFallThruBBSkippingEmpty(BB &bb) {
  ASSERT(bb.GetKind() == kBBFallthru || bb.GetKind() == kBBCondGoto, "GetFallThruSkippingEmpty: unexpected BB kind");
  ASSERT(!bb.GetSucc().empty(), "container check");
  BB *fallthru = bb.GetSucc().front();
  do {
    if (fallthru->GetPred().size() > 1 || fallthru->GetAttributes(kBBAttrIsTryEnd)) {
      return fallthru;
    }
    if (func.GetIRMap() != nullptr) {
      if (!fallthru->IsMeStmtEmpty()) {
        return fallthru;
      }
    } else {
      if (!fallthru->IsEmpty()) {
        return fallthru;
      }
    }
    laidOut[fallthru->GetBBId()] = true;
    BB *oldFallThru = fallthru;
    fallthru = fallthru->GetSucc().front();
    bb.ReplaceSucc(oldFallThru, fallthru);
    oldFallThru->RemoveBBFromPred(&bb);
    if (oldFallThru->GetPred().empty()) {
      RemoveUnreachable(*oldFallThru);
    }
  } while (true);
}

// bb end with a goto statement; remove the goto stmt if its target
// is its fallthru nextBB.
void BBLayout::ChangeToFallthruFromGoto(BB &bb) {
  ASSERT(bb.GetKind() == kBBGoto, "ChangeToFallthruFromGoto: unexpected BB kind");
  if (func.GetIRMap() != nullptr) {
    bb.RemoveMeStmt(to_ptr(bb.GetMeStmts().rbegin()));
  } else {
    bb.RemoveLastStmt();
  }
  bb.SetKind(kBBFallthru);
}

// bb does not end with a branch statement; if its fallthru is not nextBB,
// perform the fix by either laying out the fallthru immediately or adding a goto
void BBLayout::ResolveUnconditionalFallThru(BB &bb, BB &nextBB) {
  ASSERT(bb.GetKind() == kBBFallthru || bb.GetKind() == kBBGoto, "ResolveUnconditionalFallThru: unexpected BB kind");
  if (bb.GetKind() == kBBGoto) {
    return;
  }
  ASSERT(bb.GetAttributes(kBBAttrIsTry) || bb.GetAttributes(kBBAttrWontExit) || bb.GetSucc().size() == 1,
         "runtime check error");
  BB *fallthru = GetFallThruBBSkippingEmpty(bb);
  if (fallthru != &nextBB) {
    if (BBCanBeMoved(*fallthru, bb)) {
      AddBB(*fallthru);
      ResolveUnconditionalFallThru(*fallthru, nextBB);
      OptimizeBranchTarget(*fallthru);
    } else {
      CreateGoto(bb, func, *fallthru);
      OptimizeBranchTarget(bb);
    }
  }
}

// remove unnessary bb whose pred size is zero
// keep cfg correct to rebuild dominance
void BBLayout::RemoveUnreachable(BB &bb) {
  if (bb.GetAttributes(kBBAttrIsEntry)) {
    return;
  }
  MapleVector<BB*> succBBs = bb.GetSucc();
  for (BB *succ : succBBs) {
    MapleVector<BB*> preds = succ->GetPred();
    bb.RemoveBBFromVector(preds);
    if (preds.empty()) {
        RemoveUnreachable(*succ);
    }
  }
  succBBs.clear();
  func.NullifyBBByID(bb.GetBBId());
}

AnalysisResult *MeDoBBLayout::Run(MeFunction *func, MeFuncResultMgr *funcResMgr, ModuleResultMgr *moduleResMgr) {
  // mempool used in analysisresult
  MemPool *layoutMp = NewMemPool();
  auto *bbLayout = layoutMp->New<BBLayout>(*layoutMp, *func, DEBUGFUNC(func));
  // assume common_entry_bb is always bb 0
  ASSERT(func->front() == func->GetCommonEntryBB(), "assume bb[0] is the commont entry bb");
  BB *bb = func->GetFirstBB();
  while (bb != nullptr) {
    bbLayout->AddBB(*bb);
    if (bb->GetKind() == kBBCondGoto || bb->GetKind() == kBBGoto) {
      bbLayout->OptimizeBranchTarget(*bb);
    }
    BB *nextBB = bbLayout->NextBB();
    if (nextBB != nullptr) {
      // check try-endtry correspondence
      bool isTry = false;
      if (func->GetIRMap() != nullptr) {
        isTry = !nextBB->GetMeStmts().empty() && nextBB->GetMeStmts().front().GetOp() == OP_try;
      } else {
        auto &stmtNodes = nextBB->GetStmtNodes();
        isTry = !stmtNodes.empty() && stmtNodes.front().GetOpCode() == OP_try;
      }
      ASSERT(!(isTry && bbLayout->GetTryOutstanding()), "cannot emit another try if last try has not been ended");
      if (nextBB->GetAttributes(kBBAttrIsTryEnd)) {
        ASSERT(func->GetTryBBFromEndTryBB(nextBB) == nextBB ||
               bbLayout->IsBBLaidOut(func->GetTryBBFromEndTryBB(nextBB)->GetBBId()),
               "cannot emit endtry bb before its corresponding try bb");
      }
    }
    // based on nextBB, may need to fix current bb's fall-thru
    if (bb->GetKind() == kBBFallthru) {
      bbLayout->ResolveUnconditionalFallThru(*bb, *nextBB);
    } else if (bb->GetKind() == kBBCondGoto) {
      BB *oldFallThru = bb->GetSucc(0);
      BB *fallthru = bbLayout->GetFallThruBBSkippingEmpty(*bb);
      BB *brTargetBB = bb->GetSucc(1);
      if (brTargetBB != fallthru && (oldFallThru != fallthru || fallthru->GetPred().size() > 1) &&
          bbLayout->BBCanBeMoved(*brTargetBB, *bb)) {
        // flip the sense of the condgoto and lay out brTargetBB right here
        LabelIdx fallthruLabel = func->GetOrCreateBBLabel(*fallthru);
        if (func->GetIRMap() != nullptr) {
          auto &condGotoMeStmt = static_cast<CondGotoMeStmt&>(bb->GetMeStmts().back());
          ASSERT(brTargetBB->GetBBLabel() == condGotoMeStmt.GetOffset(), "bbLayout: wrong branch target BB");
          condGotoMeStmt.SetOffset(fallthruLabel);
          condGotoMeStmt.SetOp((condGotoMeStmt.GetOp() == OP_brtrue) ? OP_brfalse : OP_brtrue);
        } else {
          auto &condGotoNode = static_cast<CondGotoNode&>(bb->GetStmtNodes().back());
          ASSERT(brTargetBB->GetBBLabel() == condGotoNode.GetOffset(), "bbLayout: wrong branch target BB");
          condGotoNode.SetOffset(fallthruLabel);
          condGotoNode.SetOpCode((condGotoNode.GetOpCode() == OP_brtrue) ? OP_brfalse : OP_brtrue);
        }
        bbLayout->AddBB(*brTargetBB);
        bbLayout->ResolveUnconditionalFallThru(*brTargetBB, *nextBB);
        bbLayout->OptimizeBranchTarget(*brTargetBB);
      } else if (fallthru != nextBB) {
        if (bbLayout->BBCanBeMoved(*fallthru, *bb)) {
          bbLayout->AddBB(*fallthru);
          bbLayout->ResolveUnconditionalFallThru(*fallthru, *nextBB);
          bbLayout->OptimizeBranchTarget(*fallthru);
        } else {
          // create a new fallthru that contains a goto to the original fallthru
          BB *newFallthru = func->NewBasicBlock();
          newFallthru->SetAttributes(kBBAttrArtificial);
          bbLayout->AddLaidOut(false);
          newFallthru->SetKind(kBBGoto);
          bbLayout->SetNewBBInLayout();
          LabelIdx fallthruLabel = func->GetOrCreateBBLabel(*fallthru);
          if (func->GetIRMap() != nullptr) {
            GotoNode stmt(OP_goto);
            auto *newGoto = func->GetIRMap()->New<GotoMeStmt>(&stmt);
            newGoto->SetOffset(fallthruLabel);
            newFallthru->SetFirstMe(newGoto);
            newFallthru->SetLastMe(to_ptr(newFallthru->GetMeStmts().begin()));
          } else {
            auto *newGoto = func->GetMirFunc()->GetCodeMempool()->New<GotoNode>(OP_goto);
            newGoto->SetOffset(fallthruLabel);
            newFallthru->SetFirst(newGoto);
            newFallthru->SetLast(newFallthru->GetStmtNodes().begin().d());
          }
          // replace pred and succ
          bb->ReplaceSucc(fallthru, newFallthru);
          fallthru->ReplacePred(bb, newFallthru);
          newFallthru->SetFrequency(fallthru->GetFrequency());
          if (DEBUGFUNC(func)) {
            LogInfo::MapleLogger() << "Created fallthru and goto original fallthru" << '\n';
          }
          bbLayout->AddBB(*newFallthru);
          bbLayout->OptimizeBranchTarget(*newFallthru);
        }
      }
    }
    if (bb->GetKind() == kBBGoto) {
      // see if goto target can be placed here
      BB *gotoTarget = bb->GetSucc().front();
      CHECK_FATAL(gotoTarget != nullptr, "null ptr check");

      if (gotoTarget != nextBB && bbLayout->BBCanBeMoved(*gotoTarget, *bb)) {
        bbLayout->AddBB(*gotoTarget);
        bbLayout->ChangeToFallthruFromGoto(*bb);
        bbLayout->ResolveUnconditionalFallThru(*gotoTarget, *nextBB);
        bbLayout->OptimizeBranchTarget(*gotoTarget);
      } else if (gotoTarget->GetKind() == kBBCondGoto && gotoTarget->GetPred().size() == 1) {
        BB *targetNext = gotoTarget->GetSucc().front();
        if (targetNext != nextBB && bbLayout->BBCanBeMoved(*targetNext, *bb)) {
          bbLayout->AddBB(*gotoTarget);
          bbLayout->ChangeToFallthruFromGoto(*bb);
          bbLayout->OptimizeBranchTarget(*gotoTarget);
          bbLayout->AddBB(*targetNext);
          bbLayout->ResolveUnconditionalFallThru(*targetNext, *nextBB);
          bbLayout->OptimizeBranchTarget(*targetNext);
        }
      }
    }
    if (nextBB != nullptr && bbLayout->IsBBLaidOut(nextBB->GetBBId())) {
      nextBB = bbLayout->NextBB();
    }
    bb = nextBB;
  }
  if (bbLayout->IsNewBBInLayout()) {
    funcResMgr->InvalidAnalysisResult(MeFuncPhase_DOMINANCE, func);
  }
  if (DEBUGFUNC(func)) {
    func->GetTheCfg()->DumpToFile("afterBBLayout", false);
  }
  return bbLayout;
}
}  // namespace maple
