//===- UnsafeBufferUsage.cpp - Replace pointers with modern C++ -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/Analyses/UnsafeBufferUsage.h"
#include "clang/AST/APValue.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/AST/Expr.h"
#include "clang/AST/FormatString.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/LowLevelHelpers.h"
#include "clang/Analysis/Support/FixitUtil.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <optional>
#include <queue>
#include <set>
#include <sstream>

using namespace clang;

#ifndef NDEBUG
namespace {
class StmtDebugPrinter
    : public ConstStmtVisitor<StmtDebugPrinter, std::string> {
public:
  std::string VisitStmt(const Stmt *S) { return S->getStmtClassName(); }

  std::string VisitBinaryOperator(const BinaryOperator *BO) {
    return "BinaryOperator(" + BO->getOpcodeStr().str() + ")";
  }

  std::string VisitUnaryOperator(const UnaryOperator *UO) {
    return "UnaryOperator(" + UO->getOpcodeStr(UO->getOpcode()).str() + ")";
  }

  std::string VisitImplicitCastExpr(const ImplicitCastExpr *ICE) {
    return "ImplicitCastExpr(" + std::string(ICE->getCastKindName()) + ")";
  }
};

// Returns a string of ancestor `Stmt`s of the given `DRE` in such a form:
// "DRE ==> parent-of-DRE ==> grandparent-of-DRE ==> ...".
static std::string getDREAncestorString(const DeclRefExpr *DRE,
                                        ASTContext &Ctx) {
  std::stringstream SS;
  const Stmt *St = DRE;
  StmtDebugPrinter StmtPriner;

  do {
    SS << StmtPriner.Visit(St);

    DynTypedNodeList StParents = Ctx.getParents(*St);

    if (StParents.size() > 1)
      return "unavailable due to multiple parents";
    if (StParents.empty())
      break;
    St = StParents.begin()->get<Stmt>();
    if (St)
      SS << " ==> ";
  } while (St);
  return SS.str();
}

} // namespace
#endif /* NDEBUG */

namespace {
// Using a custom `FastMatcher` instead of ASTMatchers to achieve better
// performance. FastMatcher uses simple function `matches` to find if a node
// is a match, avoiding the dependency on the ASTMatchers framework which
// provide a nice abstraction, but incur big performance costs.
class FastMatcher {
public:
  virtual bool matches(const DynTypedNode &DynNode, ASTContext &Ctx,
                       const UnsafeBufferUsageHandler &Handler) = 0;
  virtual ~FastMatcher() = default;
};

class MatchResult {

public:
  template <typename T> const T *getNodeAs(StringRef ID) const {
    auto It = Nodes.find(ID);
    if (It == Nodes.end()) {
      return nullptr;
    }
    return It->second.get<T>();
  }

  void addNode(StringRef ID, const DynTypedNode &Node) { Nodes[ID] = Node; }

private:
  llvm::StringMap<DynTypedNode> Nodes;
};
} // namespace

#define SIZED_CONTAINER_OR_VIEW_LIST                                           \
  "span", "array", "vector", "basic_string_view", "basic_string",              \
      "initializer_list",

// A `RecursiveASTVisitor` that traverses all descendants of a given node "n"
// except for those belonging to a different callable of "n".
class MatchDescendantVisitor : public DynamicRecursiveASTVisitor {
public:
  // Creates an AST visitor that matches `Matcher` on all
  // descendants of a given node "n" except for the ones
  // belonging to a different callable of "n".
  MatchDescendantVisitor(ASTContext &Context, FastMatcher &Matcher,
                         bool FindAll, bool ignoreUnevaluatedContext,
                         const UnsafeBufferUsageHandler &NewHandler)
      : Matcher(&Matcher), FindAll(FindAll), Matches(false),
        ignoreUnevaluatedContext(ignoreUnevaluatedContext),
        ActiveASTContext(&Context), Handler(&NewHandler) {
    ShouldVisitTemplateInstantiations = true;
    ShouldVisitImplicitCode = false; // TODO: let's ignore implicit code for now
  }

  // Returns true if a match is found in a subtree of `DynNode`, which belongs
  // to the same callable of `DynNode`.
  bool findMatch(const DynTypedNode &DynNode) {
    Matches = false;
    if (const Stmt *StmtNode = DynNode.get<Stmt>()) {
      TraverseStmt(const_cast<Stmt *>(StmtNode));
      return Matches;
    }
    return false;
  }

  // The following are overriding methods from the base visitor class.
  // They are public only to allow CRTP to work. They are *not *part
  // of the public API of this class.

  // For the matchers so far used in safe buffers, we only need to match
  // `Stmt`s.  To override more as needed.

  bool TraverseDecl(Decl *Node) override {
    if (!Node)
      return true;
    if (!match(*Node))
      return false;
    // To skip callables:
    if (isa<FunctionDecl, BlockDecl, ObjCMethodDecl>(Node))
      return true;
    // Traverse descendants
    return DynamicRecursiveASTVisitor::TraverseDecl(Node);
  }

  bool TraverseGenericSelectionExpr(GenericSelectionExpr *Node) override {
    // These are unevaluated, except the result expression.
    if (ignoreUnevaluatedContext)
      return TraverseStmt(Node->getResultExpr());
    return DynamicRecursiveASTVisitor::TraverseGenericSelectionExpr(Node);
  }

  bool
  TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *Node) override {
    // Unevaluated context.
    if (ignoreUnevaluatedContext)
      return true;
    return DynamicRecursiveASTVisitor::TraverseUnaryExprOrTypeTraitExpr(Node);
  }

  bool TraverseTypeOfExprTypeLoc(TypeOfExprTypeLoc Node) override {
    // Unevaluated context.
    if (ignoreUnevaluatedContext)
      return true;
    return DynamicRecursiveASTVisitor::TraverseTypeOfExprTypeLoc(Node);
  }

  bool TraverseDecltypeTypeLoc(DecltypeTypeLoc Node) override {
    // Unevaluated context.
    if (ignoreUnevaluatedContext)
      return true;
    return DynamicRecursiveASTVisitor::TraverseDecltypeTypeLoc(Node);
  }

  bool TraverseCXXNoexceptExpr(CXXNoexceptExpr *Node) override {
    // Unevaluated context.
    if (ignoreUnevaluatedContext)
      return true;
    return DynamicRecursiveASTVisitor::TraverseCXXNoexceptExpr(Node);
  }

  bool TraverseCXXTypeidExpr(CXXTypeidExpr *Node) override {
    // Unevaluated context.
    if (ignoreUnevaluatedContext)
      return true;
    return DynamicRecursiveASTVisitor::TraverseCXXTypeidExpr(Node);
  }

  bool TraverseCXXDefaultInitExpr(CXXDefaultInitExpr *Node) override {
    if (!TraverseStmt(Node->getExpr()))
      return false;
    return DynamicRecursiveASTVisitor::TraverseCXXDefaultInitExpr(Node);
  }

  bool TraverseStmt(Stmt *Node) override {
    if (!Node)
      return true;
    if (!match(*Node))
      return false;
    return DynamicRecursiveASTVisitor::TraverseStmt(Node);
  }

private:
  // Sets 'Matched' to true if 'Matcher' matches 'Node'
  //
  // Returns 'true' if traversal should continue after this function
  // returns, i.e. if no match is found or 'Bind' is 'BK_All'.
  template <typename T> bool match(const T &Node) {
    if (Matcher->matches(DynTypedNode::create(Node), *ActiveASTContext,
                         *Handler)) {
      Matches = true;
      if (!FindAll)
        return false; // Abort as soon as a match is found.
    }
    return true;
  }

  FastMatcher *const Matcher;
  // When true, finds all matches. When false, finds the first match and stops.
  const bool FindAll;
  bool Matches;
  bool ignoreUnevaluatedContext;
  ASTContext *ActiveASTContext;
  const UnsafeBufferUsageHandler *Handler;
};

// Because we're dealing with raw pointers, let's define what we mean by that.
static bool hasPointerType(const Expr &E) {
  return isa<PointerType>(E.getType().getCanonicalType());
}

static bool hasArrayType(const Expr &E) {
  return isa<ArrayType>(E.getType().getCanonicalType());
}

static void
forEachDescendantEvaluatedStmt(const Stmt *S, ASTContext &Ctx,
                               const UnsafeBufferUsageHandler &Handler,
                               FastMatcher &Matcher) {
  MatchDescendantVisitor Visitor(Ctx, Matcher, /*FindAll=*/true,
                                 /*ignoreUnevaluatedContext=*/true, Handler);
  Visitor.findMatch(DynTypedNode::create(*S));
}

static void forEachDescendantStmt(const Stmt *S, ASTContext &Ctx,
                                  const UnsafeBufferUsageHandler &Handler,
                                  FastMatcher &Matcher) {
  MatchDescendantVisitor Visitor(Ctx, Matcher, /*FindAll=*/true,
                                 /*ignoreUnevaluatedContext=*/false, Handler);
  Visitor.findMatch(DynTypedNode::create(*S));
}

// Matches a `Stmt` node iff the node is in a safe-buffer opt-out region
static bool notInSafeBufferOptOut(const Stmt &Node,
                                  const UnsafeBufferUsageHandler *Handler) {
  return !Handler->isSafeBufferOptOut(Node.getBeginLoc());
}

static bool
ignoreUnsafeBufferInContainer(const Stmt &Node,
                              const UnsafeBufferUsageHandler *Handler) {
  return Handler->ignoreUnsafeBufferInContainer(Node.getBeginLoc());
}

static bool ignoreUnsafeLibcCall(const ASTContext &Ctx, const Stmt &Node,
                                 const UnsafeBufferUsageHandler *Handler) {
  if (Ctx.getLangOpts().CPlusPlus)
    return Handler->ignoreUnsafeBufferInLibcCall(Node.getBeginLoc());
  return true; /* Only warn about libc calls for C++ */
}

// Finds any expression 'e' such that `OnResult`
// matches 'e' and 'e' is in an Unspecified Lvalue Context.
static void findStmtsInUnspecifiedLvalueContext(
    const Stmt *S, const llvm::function_ref<void(const Expr *)> OnResult) {
  if (const auto *CE = dyn_cast<ImplicitCastExpr>(S);
      CE && CE->getCastKind() == CastKind::CK_LValueToRValue)
    OnResult(CE->getSubExpr());
  if (const auto *BO = dyn_cast<BinaryOperator>(S);
      BO && BO->getOpcode() == BO_Assign)
    OnResult(BO->getLHS());
}

// Finds any expression `e` such that `InnerMatcher` matches `e` and
// `e` is in an Unspecified Pointer Context (UPC).
static void findStmtsInUnspecifiedPointerContext(
    const Stmt *S, llvm::function_ref<void(const Stmt *)> InnerMatcher) {
  // A UPC can be
  // 1. an argument of a function call (except the callee has [[unsafe_...]]
  //    attribute), or
  // 2. the operand of a pointer-to-(integer or bool) cast operation; or
  // 3. the operand of a comparator operation; or
  // 4. the operand of a pointer subtraction operation
  //    (i.e., computing the distance between two pointers); or ...

  if (auto *CE = dyn_cast<CallExpr>(S)) {
    if (const auto *FnDecl = CE->getDirectCallee();
        FnDecl && FnDecl->hasAttr<UnsafeBufferUsageAttr>())
      return;
    ast_matchers::matchEachArgumentWithParamType(
        *CE, [&InnerMatcher](QualType Type, const Expr *Arg) {
          if (Type->isAnyPointerType())
            InnerMatcher(Arg);
        });
  }

  if (auto *CE = dyn_cast<CastExpr>(S)) {
    if (CE->getCastKind() != CastKind::CK_PointerToIntegral &&
        CE->getCastKind() != CastKind::CK_PointerToBoolean)
      return;
    if (!hasPointerType(*CE->getSubExpr()))
      return;
    InnerMatcher(CE->getSubExpr());
  }

  // Pointer comparison operator.
  if (const auto *BO = dyn_cast<BinaryOperator>(S);
      BO && (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE ||
             BO->getOpcode() == BO_LT || BO->getOpcode() == BO_LE ||
             BO->getOpcode() == BO_GT || BO->getOpcode() == BO_GE)) {
    auto *LHS = BO->getLHS();
    if (hasPointerType(*LHS))
      InnerMatcher(LHS);

    auto *RHS = BO->getRHS();
    if (hasPointerType(*RHS))
      InnerMatcher(RHS);
  }

  // Pointer subtractions.
  if (const auto *BO = dyn_cast<BinaryOperator>(S);
      BO && BO->getOpcode() == BO_Sub && hasPointerType(*BO->getLHS()) &&
      hasPointerType(*BO->getRHS())) {
    // Note that here we need both LHS and RHS to be
    // pointer. Then the inner matcher can match any of
    // them:
    InnerMatcher(BO->getLHS());
    InnerMatcher(BO->getRHS());
  }
  // FIXME: any more cases? (UPC excludes the RHS of an assignment.  For now
  // we don't have to check that.)
}

// Finds statements in unspecified untyped context i.e. any expression 'e' such
// that `InnerMatcher` matches 'e' and 'e' is in an unspecified untyped context
// (i.e the expression 'e' isn't evaluated to an RValue). For example, consider
// the following code:
//    int *p = new int[4];
//    int *q = new int[4];
//    if ((p = q)) {}
//    p = q;
// The expression `p = q` in the conditional of the `if` statement
// `if ((p = q))` is evaluated as an RValue, whereas the expression `p = q;`
// in the assignment statement is in an untyped context.
static void findStmtsInUnspecifiedUntypedContext(
    const Stmt *S, llvm::function_ref<void(const Stmt *)> InnerMatcher) {
  // An unspecified context can be
  // 1. A compound statement,
  // 2. The body of an if statement
  // 3. Body of a loop
  if (auto *CS = dyn_cast<CompoundStmt>(S)) {
    for (auto *Child : CS->body())
      InnerMatcher(Child);
  }
  if (auto *IfS = dyn_cast<IfStmt>(S)) {
    if (IfS->getThen())
      InnerMatcher(IfS->getThen());
    if (IfS->getElse())
      InnerMatcher(IfS->getElse());
  }
  // FIXME: Handle loop bodies.
}

// Returns true iff integer E1 is equivalent to integer E2.
//
// For now we only support such expressions:
//    expr := DRE | const-value | expr BO expr
//    BO   := '*' | '+'
//
// FIXME: We can reuse the expression comparator of the interop analysis after
// it has been upstreamed.
static bool areEqualIntegers(const Expr *E1, const Expr *E2, ASTContext &Ctx);
static bool areEqualIntegralBinaryOperators(const BinaryOperator *E1,
                                            const Expr *E2_LHS,
                                            BinaryOperatorKind BOP,
                                            const Expr *E2_RHS,
                                            ASTContext &Ctx) {
  if (E1->getOpcode() == BOP) {
    switch (BOP) {
      // Commutative operators:
    case BO_Mul:
    case BO_Add:
      return (areEqualIntegers(E1->getLHS(), E2_LHS, Ctx) &&
              areEqualIntegers(E1->getRHS(), E2_RHS, Ctx)) ||
             (areEqualIntegers(E1->getLHS(), E2_RHS, Ctx) &&
              areEqualIntegers(E1->getRHS(), E2_LHS, Ctx));
    default:
      return false;
    }
  }
  return false;
}

static bool areEqualIntegers(const Expr *E1, const Expr *E2, ASTContext &Ctx) {
  E1 = E1->IgnoreParenImpCasts();
  E2 = E2->IgnoreParenImpCasts();
  if (!E1->getType()->isIntegerType() || E1->getType() != E2->getType())
    return false;

  Expr::EvalResult ER1, ER2;

  // If both are constants:
  if (E1->EvaluateAsInt(ER1, Ctx) && E2->EvaluateAsInt(ER2, Ctx))
    return ER1.Val.getInt() == ER2.Val.getInt();

  // Otherwise, they should have identical stmt kind:
  if (E1->getStmtClass() != E2->getStmtClass())
    return false;
  switch (E1->getStmtClass()) {
  case Stmt::DeclRefExprClass:
    return cast<DeclRefExpr>(E1)->getDecl() == cast<DeclRefExpr>(E2)->getDecl();
  case Stmt::BinaryOperatorClass: {
    auto BO2 = cast<BinaryOperator>(E2);
    return areEqualIntegralBinaryOperators(cast<BinaryOperator>(E1),
                                           BO2->getLHS(), BO2->getOpcode(),
                                           BO2->getRHS(), Ctx);
  }
  default:
    return false;
  }
}

// Providing that `Ptr` is a pointer and `Size` is an unsigned-integral
// expression, returns true iff they follow one of the following safe
// patterns:
//  1. Ptr is `DRE.data()` and Size is `DRE.size()`, where DRE is a hardened
//     container or view;
//
//  2. Ptr is `a` and Size is `n`, where `a` is of an array-of-T with constant
//     size `n`;
//
//  3. Ptr is `&var` and Size is `1`; or
//     Ptr is `std::addressof(...)` and Size is `1`;
//
//  4. Size is `0`;
static bool isPtrBufferSafe(const Expr *Ptr, const Expr *Size,
                            ASTContext &Ctx) {
  // Pattern 1:
  if (auto *MCEPtr = dyn_cast<CXXMemberCallExpr>(Ptr->IgnoreParenImpCasts()))
    if (auto *MCESize =
            dyn_cast<CXXMemberCallExpr>(Size->IgnoreParenImpCasts())) {
      auto *DREOfPtr = dyn_cast<DeclRefExpr>(
          MCEPtr->getImplicitObjectArgument()->IgnoreParenImpCasts());
      auto *DREOfSize = dyn_cast<DeclRefExpr>(
          MCESize->getImplicitObjectArgument()->IgnoreParenImpCasts());

      if (!DREOfPtr || !DREOfSize)
        return false; // not in safe pattern
      // We need to make sure 'a' is identical to 'b' for 'a.data()' and
      // 'b.size()' otherwise we do not know they match:
      if (DREOfPtr->getDecl() != DREOfSize->getDecl())
        return false;
      if (MCEPtr->getMethodDecl()->getName() != "data")
        return false;
      // `MCEPtr->getRecordDecl()` must be non-null as `DREOfPtr` is non-null:
      if (!MCEPtr->getRecordDecl()->isInStdNamespace())
        return false;

      auto *ObjII = MCEPtr->getRecordDecl()->getIdentifier();

      if (!ObjII)
        return false;

      bool AcceptSizeBytes = Ptr->getType()->getPointeeType()->isCharType();

      if (!((AcceptSizeBytes &&
             MCESize->getMethodDecl()->getName() == "size_bytes") ||
            // Note here the pointer must be a pointer-to-char type unless there
            // is explicit casting.  If there is explicit casting, this branch
            // is unreachable. Thus, at this branch "size" and "size_bytes" are
            // equivalent as the pointer is a char pointer:
            MCESize->getMethodDecl()->getName() == "size"))
        return false;

      return llvm::is_contained({SIZED_CONTAINER_OR_VIEW_LIST},
                                ObjII->getName());
    }

  Expr::EvalResult ER;

  // Pattern 2-4:
  if (Size->EvaluateAsInt(ER, Ctx)) {
    // Pattern 2:
    if (auto *DRE = dyn_cast<DeclRefExpr>(Ptr->IgnoreParenImpCasts())) {
      if (auto *CAT = Ctx.getAsConstantArrayType(DRE->getType())) {
        llvm::APSInt SizeInt = ER.Val.getInt();

        return llvm::APSInt::compareValues(
                   SizeInt, llvm::APSInt(CAT->getSize(), true)) == 0;
      }
      return false;
    }

    // Pattern 3:
    if (ER.Val.getInt().isOne()) {
      if (auto *UO = dyn_cast<UnaryOperator>(Ptr->IgnoreParenImpCasts()))
        return UO && UO->getOpcode() == UnaryOperator::Opcode::UO_AddrOf;
      if (auto *CE = dyn_cast<CallExpr>(Ptr->IgnoreParenImpCasts())) {
        auto *FnDecl = CE->getDirectCallee();

        return FnDecl && FnDecl->getNameAsString() == "addressof" &&
               FnDecl->isInStdNamespace();
      }
      return false;
    }
    // Pattern 4:
    if (ER.Val.getInt().isZero())
      return true;
  }
  return false;
}

// Given a two-param std::span construct call, matches iff the call has the
// following forms:
//   1. `std::span<T>{new T[n], n}`, where `n` is a literal or a DRE
//   2. `std::span<T>{new T, 1}`
//   3. `std::span<T>{ (char *)f(args), args[N] * arg*[M]}`, where
//       `f` is a function with attribute `alloc_size(N, M)`;
//       `args` represents the list of arguments;
//       `N, M` are parameter indexes to the allocating element number and size.
//        Sometimes, there is only one parameter index representing the total
//        size.
//   4. `std::span<T>{x.begin(), x.end()}` where `x` is an object in the
//      SIZED_CONTAINER_OR_VIEW_LIST.
//   5. `isPtrBufferSafe` returns true for the two arguments of the span
//      constructor
static bool isSafeSpanTwoParamConstruct(const CXXConstructExpr &Node,
                                        ASTContext &Ctx) {
  assert(Node.getNumArgs() == 2 &&
         "expecting a two-parameter std::span constructor");
  const Expr *Arg0 = Node.getArg(0)->IgnoreParenImpCasts();
  const Expr *Arg1 = Node.getArg(1)->IgnoreParenImpCasts();
  auto HaveEqualConstantValues = [&Ctx](const Expr *E0, const Expr *E1) {
    if (auto E0CV = E0->getIntegerConstantExpr(Ctx))
      if (auto E1CV = E1->getIntegerConstantExpr(Ctx)) {
        return llvm::APSInt::compareValues(*E0CV, *E1CV) == 0;
      }
    return false;
  };
  auto AreSameDRE = [](const Expr *E0, const Expr *E1) {
    if (auto *DRE0 = dyn_cast<DeclRefExpr>(E0))
      if (auto *DRE1 = dyn_cast<DeclRefExpr>(E1)) {
        return DRE0->getDecl() == DRE1->getDecl();
      }
    return false;
  };
  std::optional<llvm::APSInt> Arg1CV = Arg1->getIntegerConstantExpr(Ctx);

  if (Arg1CV && Arg1CV->isZero())
    // Check form 5:
    return true;

  // Check forms 1-2:
  switch (Arg0->getStmtClass()) {
  case Stmt::CXXNewExprClass:
    if (auto Size = cast<CXXNewExpr>(Arg0)->getArraySize()) {
      // Check form 1:
      return AreSameDRE((*Size)->IgnoreImplicit(), Arg1) ||
             HaveEqualConstantValues(*Size, Arg1);
    }
    // TODO: what's placeholder type? avoid it for now.
    if (!cast<CXXNewExpr>(Arg0)->hasPlaceholderType()) {
      // Check form 2:
      return Arg1CV && Arg1CV->isOne();
    }
    break;
  default:
    break;
  }

  // Check form 3:
  if (auto CCast = dyn_cast<CStyleCastExpr>(Arg0)) {
    if (!CCast->getType()->isPointerType())
      return false;

    QualType PteTy = CCast->getType()->getPointeeType();

    if (!(PteTy->isConstantSizeType() && Ctx.getTypeSizeInChars(PteTy).isOne()))
      return false;

    if (const auto *Call = dyn_cast<CallExpr>(CCast->getSubExpr())) {
      if (const FunctionDecl *FD = Call->getDirectCallee())
        if (auto *AllocAttr = FD->getAttr<AllocSizeAttr>()) {
          const Expr *EleSizeExpr =
              Call->getArg(AllocAttr->getElemSizeParam().getASTIndex());
          // NumElemIdx is invalid if AllocSizeAttr has 1 argument:
          ParamIdx NumElemIdx = AllocAttr->getNumElemsParam();

          if (!NumElemIdx.isValid())
            return areEqualIntegers(Arg1, EleSizeExpr, Ctx);

          const Expr *NumElesExpr = Call->getArg(NumElemIdx.getASTIndex());

          if (auto BO = dyn_cast<BinaryOperator>(Arg1))
            return areEqualIntegralBinaryOperators(BO, NumElesExpr, BO_Mul,
                                                   EleSizeExpr, Ctx);
        }
    }
  }
  // Check form 4:
  auto IsMethodCallToSizedObject = [](const Stmt *Node, StringRef MethodName) {
    if (const auto *MC = dyn_cast<CXXMemberCallExpr>(Node)) {
      const auto *MD = MC->getMethodDecl();
      const auto *RD = MC->getRecordDecl();

      if (RD && MD)
        if (auto *II = RD->getDeclName().getAsIdentifierInfo();
            II && RD->isInStdNamespace())
          return llvm::is_contained({SIZED_CONTAINER_OR_VIEW_LIST},
                                    II->getName()) &&
                 MD->getName() == MethodName;
    }
    return false;
  };

  if (IsMethodCallToSizedObject(Arg0, "begin") &&
      IsMethodCallToSizedObject(Arg1, "end"))
    return AreSameDRE(
        // We know Arg0 and Arg1 are `CXXMemberCallExpr`s:
        cast<CXXMemberCallExpr>(Arg0)
            ->getImplicitObjectArgument()
            ->IgnoreParenImpCasts(),
        cast<CXXMemberCallExpr>(Arg1)
            ->getImplicitObjectArgument()
            ->IgnoreParenImpCasts());

  // Check 5:
  return isPtrBufferSafe(Arg0, Arg1, Ctx);
}

static bool isSafeArraySubscript(const ArraySubscriptExpr &Node,
                                 const ASTContext &Ctx) {
  // FIXME: Proper solution:
  //  - refactor Sema::CheckArrayAccess
  //    - split safe/OOB/unknown decision logic from diagnostics emitting code
  //    -  e. g. "Try harder to find a NamedDecl to point at in the note."
  //    already duplicated
  //  - call both from Sema and from here

  uint64_t limit;
  if (const auto *CATy =
          dyn_cast<ConstantArrayType>(Node.getBase()
                                          ->IgnoreParenImpCasts()
                                          ->getType()
                                          ->getUnqualifiedDesugaredType())) {
    limit = CATy->getLimitedSize();
  } else if (const auto *SLiteral = dyn_cast<clang::StringLiteral>(
                 Node.getBase()->IgnoreParenImpCasts())) {
    limit = SLiteral->getLength() + 1;
  } else {
    return false;
  }

  Expr::EvalResult EVResult;
  const Expr *IndexExpr = Node.getIdx();
  if (!IndexExpr->isValueDependent() &&
      IndexExpr->EvaluateAsInt(EVResult, Ctx)) {
    llvm::APSInt ArrIdx = EVResult.Val.getInt();
    // FIXME: ArrIdx.isNegative() we could immediately emit an error as that's a
    // bug
    if (ArrIdx.isNonNegative() && ArrIdx.getLimitedValue() < limit)
      return true;
  } else if (const auto *BE = dyn_cast<BinaryOperator>(IndexExpr)) {
    // For an integer expression `e` and an integer constant `n`, `e & n` and
    // `n & e` are bounded by `n`:
    if (BE->getOpcode() != BO_And && BE->getOpcode() != BO_Rem)
      return false;

    const Expr *LHS = BE->getLHS();
    const Expr *RHS = BE->getRHS();

    if (BE->getOpcode() == BO_Rem) {
      // If n is a negative number, then n % const can be greater than const
      if (!LHS->getType()->isUnsignedIntegerType()) {
        return false;
      }

      if (!RHS->isValueDependent() && RHS->EvaluateAsInt(EVResult, Ctx)) {
        llvm::APSInt result = EVResult.Val.getInt();
        if (result.isNonNegative() && result.getLimitedValue() <= limit)
          return true;
      }

      return false;
    }

    if ((!LHS->isValueDependent() &&
         LHS->EvaluateAsInt(EVResult, Ctx)) || // case: `n & e`
        (!RHS->isValueDependent() &&
         RHS->EvaluateAsInt(EVResult, Ctx))) { // `e & n`
      llvm::APSInt result = EVResult.Val.getInt();
      if (result.isNonNegative() && result.getLimitedValue() < limit)
        return true;
    }
    return false;
  }
  return false;
}

namespace libc_func_matchers {
// Under `libc_func_matchers`, define a set of matchers that match unsafe
// functions in libc and unsafe calls to them.

//  A tiny parser to strip off common prefix and suffix of libc function names
//  in real code.
//
//  Given a function name, `matchName` returns `CoreName` according to the
//  following grammar:
//
//  LibcName     := CoreName | CoreName + "_s"
//  MatchingName := "__builtin_" + LibcName              |
//                  "__builtin___" + LibcName + "_chk"   |
//                  "__asan_" + LibcName
//
struct LibcFunNamePrefixSuffixParser {
  StringRef matchName(StringRef FunName, bool isBuiltin) {
    // Try to match __builtin_:
    if (isBuiltin && FunName.starts_with("__builtin_"))
      // Then either it is __builtin_LibcName or __builtin___LibcName_chk or
      // no match:
      return matchLibcNameOrBuiltinChk(
          FunName.drop_front(10 /* truncate "__builtin_" */));
    // Try to match __asan_:
    if (FunName.starts_with("__asan_"))
      return matchLibcName(FunName.drop_front(7 /* truncate of "__asan_" */));
    return matchLibcName(FunName);
  }

  // Parameter `Name` is the substring after stripping off the prefix
  // "__builtin_".
  StringRef matchLibcNameOrBuiltinChk(StringRef Name) {
    if (Name.starts_with("__") && Name.ends_with("_chk"))
      return matchLibcName(
          Name.drop_front(2).drop_back(4) /* truncate "__" and "_chk" */);
    return matchLibcName(Name);
  }

  StringRef matchLibcName(StringRef Name) {
    if (Name.ends_with("_s"))
      return Name.drop_back(2 /* truncate "_s" */);
    return Name;
  }
};

// A pointer type expression is known to be null-terminated, if it has the
// form: E.c_str(), for any expression E of `std::string` type.
static bool isNullTermPointer(const Expr *Ptr) {
  if (isa<clang::StringLiteral>(Ptr->IgnoreParenImpCasts()))
    return true;
  if (isa<PredefinedExpr>(Ptr->IgnoreParenImpCasts()))
    return true;
  if (auto *MCE = dyn_cast<CXXMemberCallExpr>(Ptr->IgnoreParenImpCasts())) {
    const CXXMethodDecl *MD = MCE->getMethodDecl();
    const CXXRecordDecl *RD = MCE->getRecordDecl()->getCanonicalDecl();

    if (MD && RD && RD->isInStdNamespace() && MD->getIdentifier())
      if (MD->getName() == "c_str" && RD->getName() == "basic_string")
        return true;
  }
  return false;
}

// Return true iff at least one of following cases holds:
//  1. Format string is a literal and there is an unsafe pointer argument
//     corresponding to an `s` specifier;
//  2. Format string is not a literal and there is least an unsafe pointer
//     argument (including the formatter argument).
//
// `UnsafeArg` is the output argument that will be set only if this function
// returns true.
static bool hasUnsafeFormatOrSArg(const CallExpr *Call, const Expr *&UnsafeArg,
                                  const unsigned FmtArgIdx, ASTContext &Ctx,
                                  bool isKprintf = false) {
  class StringFormatStringHandler
      : public analyze_format_string::FormatStringHandler {
    const CallExpr *Call;
    unsigned FmtArgIdx;
    const Expr *&UnsafeArg;
    ASTContext &Ctx;

    // Returns an `Expr` representing the precision if specified, null
    // otherwise.
    // The parameter `Call` is a printf call and the parameter `Precision` is
    // the precision of a format specifier of the `Call`.
    //
    // For example, for the `printf("%d, %.10s", 10, p)` call
    // `Precision` can be the precision of either "%d" or "%.10s". The former
    // one will have `NotSpecified` kind.
    const Expr *
    getPrecisionAsExpr(const analyze_printf::OptionalAmount &Precision,
                       const CallExpr *Call) {
      unsigned PArgIdx = -1;

      if (Precision.hasDataArgument())
        PArgIdx = Precision.getPositionalArgIndex() + FmtArgIdx;
      if (0 < PArgIdx && PArgIdx < Call->getNumArgs()) {
        const Expr *PArg = Call->getArg(PArgIdx);

        // Strip the cast if `PArg` is a cast-to-int expression:
        if (auto *CE = dyn_cast<CastExpr>(PArg);
            CE && CE->getType()->isSignedIntegerType())
          PArg = CE->getSubExpr();
        return PArg;
      }
      if (Precision.getHowSpecified() ==
          analyze_printf::OptionalAmount::HowSpecified::Constant) {
        auto SizeTy = Ctx.getSizeType();
        llvm::APSInt PArgVal = llvm::APSInt(
            llvm::APInt(Ctx.getTypeSize(SizeTy), Precision.getConstantAmount()),
            true);

        return IntegerLiteral::Create(Ctx, PArgVal, Ctx.getSizeType(), {});
      }
      return nullptr;
    }

  public:
    StringFormatStringHandler(const CallExpr *Call, unsigned FmtArgIdx,
                              const Expr *&UnsafeArg, ASTContext &Ctx)
        : Call(Call), FmtArgIdx(FmtArgIdx), UnsafeArg(UnsafeArg), Ctx(Ctx) {}

    bool HandlePrintfSpecifier(const analyze_printf::PrintfSpecifier &FS,
                               const char *startSpecifier,
                               unsigned specifierLen,
                               const TargetInfo &Target) override {
      if (FS.getConversionSpecifier().getKind() !=
          analyze_printf::PrintfConversionSpecifier::sArg)
        return true; // continue parsing

      unsigned ArgIdx = FS.getPositionalArgIndex() + FmtArgIdx;

      if (!(0 < ArgIdx && ArgIdx < Call->getNumArgs()))
        // If the `ArgIdx` is invalid, give up.
        return true; // continue parsing

      const Expr *Arg = Call->getArg(ArgIdx);

      if (isNullTermPointer(Arg))
        // If Arg is a null-terminated pointer, it is safe anyway.
        return true; // continue parsing

      // Otherwise, check if the specifier has a precision and if the character
      // pointer is safely bound by the precision:
      auto LengthModifier = FS.getLengthModifier();
      QualType ArgType = Arg->getType();
      bool IsArgTypeValid = // Is ArgType a character pointer type?
          ArgType->isPointerType() &&
          (LengthModifier.getKind() == LengthModifier.AsWideChar
               ? ArgType->getPointeeType()->isWideCharType()
               : ArgType->getPointeeType()->isCharType());

      if (auto *Precision = getPrecisionAsExpr(FS.getPrecision(), Call);
          Precision && IsArgTypeValid)
        if (isPtrBufferSafe(Arg, Precision, Ctx))
          return true;
      // Handle unsafe case:
      UnsafeArg = Call->getArg(ArgIdx); // output
      return false; // returning false stops parsing immediately
    }
  };

  const Expr *Fmt = Call->getArg(FmtArgIdx);

  if (auto *SL = dyn_cast<clang::StringLiteral>(Fmt->IgnoreParenImpCasts())) {
    StringRef FmtStr;

    if (SL->getCharByteWidth() == 1)
      FmtStr = SL->getString();
    else if (auto EvaledFmtStr = SL->tryEvaluateString(Ctx))
      FmtStr = *EvaledFmtStr;
    else
      goto CHECK_UNSAFE_PTR;

    StringFormatStringHandler Handler(Call, FmtArgIdx, UnsafeArg, Ctx);

    return analyze_format_string::ParsePrintfString(
        Handler, FmtStr.begin(), FmtStr.end(), Ctx.getLangOpts(),
        Ctx.getTargetInfo(), isKprintf);
  }
CHECK_UNSAFE_PTR:
  // If format is not a string literal, we cannot analyze the format string.
  // In this case, this call is considered unsafe if at least one argument
  // (including the format argument) is unsafe pointer.
  return llvm::any_of(
      llvm::make_range(Call->arg_begin() + FmtArgIdx, Call->arg_end()),
      [&UnsafeArg](const Expr *Arg) -> bool {
        if (Arg->getType()->isPointerType() && !isNullTermPointer(Arg)) {
          UnsafeArg = Arg;
          return true;
        }
        return false;
      });
}

// Matches a FunctionDecl node such that
//  1. It's name, after stripping off predefined prefix and suffix, is
//     `CoreName`; and
//  2. `CoreName` or `CoreName[str/wcs]` is one of the `PredefinedNames`, which
//     is a set of libc function names.
//
//  Note: For predefined prefix and suffix, see `LibcFunNamePrefixSuffixParser`.
//  The notation `CoreName[str/wcs]` means a new name obtained from replace
//  string "wcs" with "str" in `CoreName`.
static bool isPredefinedUnsafeLibcFunc(const FunctionDecl &Node) {
  static std::unique_ptr<std::set<StringRef>> PredefinedNames = nullptr;
  if (!PredefinedNames)
    PredefinedNames =
        std::make_unique<std::set<StringRef>, std::set<StringRef>>({
            // numeric conversion:
            "atof",
            "atoi",
            "atol",
            "atoll",
            "strtol",
            "strtoll",
            "strtoul",
            "strtoull",
            "strtof",
            "strtod",
            "strtold",
            "strtoimax",
            "strtoumax",
            // "strfromf",  "strfromd", "strfroml", // C23?
            // string manipulation:
            "strcpy",
            "strncpy",
            "strlcpy",
            "strcat",
            "strncat",
            "strlcat",
            "strxfrm",
            "strdup",
            "strndup",
            // string examination:
            "strlen",
            "strnlen",
            "strcmp",
            "strncmp",
            "stricmp",
            "strcasecmp",
            "strcoll",
            "strchr",
            "strrchr",
            "strspn",
            "strcspn",
            "strpbrk",
            "strstr",
            "strtok",
            // "mem-" functions
            "memchr",
            "wmemchr",
            "memcmp",
            "wmemcmp",
            "memcpy",
            "memccpy",
            "mempcpy",
            "wmemcpy",
            "memmove",
            "wmemmove",
            "memset",
            "wmemset",
            // IO:
            "fread",
            "fwrite",
            "fgets",
            "fgetws",
            "gets",
            "fputs",
            "fputws",
            "puts",
            // others
            "strerror_s",
            "strerror_r",
            "bcopy",
            "bzero",
            "bsearch",
            "qsort",
        });

  auto *II = Node.getIdentifier();

  if (!II)
    return false;

  StringRef Name = LibcFunNamePrefixSuffixParser().matchName(
      II->getName(), Node.getBuiltinID());

  // Match predefined names:
  if (PredefinedNames->find(Name) != PredefinedNames->end())
    return true;

  std::string NameWCS = Name.str();
  size_t WcsPos = NameWCS.find("wcs");

  while (WcsPos != std::string::npos) {
    NameWCS[WcsPos++] = 's';
    NameWCS[WcsPos++] = 't';
    NameWCS[WcsPos++] = 'r';
    WcsPos = NameWCS.find("wcs", WcsPos);
  }
  if (PredefinedNames->find(NameWCS) != PredefinedNames->end())
    return true;
  // All `scanf` functions are unsafe (including `sscanf`, `vsscanf`, etc.. They
  // all should end with "scanf"):
  return Name.ends_with("scanf");
}

// Match a call to one of the `v*printf` functions taking `va_list`.  We cannot
// check safety for these functions so they should be changed to their
// non-va_list versions.
static bool isUnsafeVaListPrintfFunc(const FunctionDecl &Node) {
  auto *II = Node.getIdentifier();

  if (!II)
    return false;

  StringRef Name = LibcFunNamePrefixSuffixParser().matchName(
      II->getName(), Node.getBuiltinID());

  if (!Name.ends_with("printf"))
    return false; // neither printf nor scanf
  return Name.starts_with("v");
}

// Matches a call to one of the `sprintf` functions as they are always unsafe
// and should be changed to `snprintf`.
static bool isUnsafeSprintfFunc(const FunctionDecl &Node) {
  auto *II = Node.getIdentifier();

  if (!II)
    return false;

  StringRef Name = LibcFunNamePrefixSuffixParser().matchName(
      II->getName(), Node.getBuiltinID());

  if (!Name.ends_with("printf") ||
      // Let `isUnsafeVaListPrintfFunc` check for cases with va-list:
      Name.starts_with("v"))
    return false;

  StringRef Prefix = Name.drop_back(6);

  if (Prefix.ends_with("w"))
    Prefix = Prefix.drop_back(1);
  return Prefix == "s";
}

// Match function declarations of `printf`, `fprintf`, `snprintf` and their wide
// character versions.  Calls to these functions can be safe if their arguments
// are carefully made safe.
static bool isNormalPrintfFunc(const FunctionDecl &Node) {
  auto *II = Node.getIdentifier();

  if (!II)
    return false;

  StringRef Name = LibcFunNamePrefixSuffixParser().matchName(
      II->getName(), Node.getBuiltinID());

  if (!Name.ends_with("printf") || Name.starts_with("v"))
    return false;

  StringRef Prefix = Name.drop_back(6);

  if (Prefix.ends_with("w"))
    Prefix = Prefix.drop_back(1);

  return Prefix.empty() || Prefix == "k" || Prefix == "f" || Prefix == "sn";
}

// This matcher requires that it is known that the callee `isNormalPrintf`.
// Then if the format string is a string literal, this matcher matches when at
// least one string argument is unsafe. If the format is not a string literal,
// this matcher matches when at least one pointer type argument is unsafe.
static bool hasUnsafePrintfStringArg(const CallExpr &Node, ASTContext &Ctx,
                                     MatchResult &Result, llvm::StringRef Tag) {
  // Determine what printf it is by examining formal parameters:
  const FunctionDecl *FD = Node.getDirectCallee();

  assert(FD && "It should have been checked that FD is non-null.");

  unsigned NumParms = FD->getNumParams();

  if (NumParms < 1)
    return false; // possibly some user-defined printf function

  QualType FirstParmTy = FD->getParamDecl(0)->getType();

  if (!FirstParmTy->isPointerType())
    return false; // possibly some user-defined printf function

  QualType FirstPteTy = FirstParmTy->castAs<PointerType>()->getPointeeType();

  if (!Ctx.getFILEType()
           .isNull() && //`FILE *` must be in the context if it is fprintf
      FirstPteTy.getCanonicalType() == Ctx.getFILEType().getCanonicalType()) {
    // It is a fprintf:
    const Expr *UnsafeArg;

    if (hasUnsafeFormatOrSArg(&Node, UnsafeArg, 1, Ctx, false)) {
      Result.addNode(Tag, DynTypedNode::create(*UnsafeArg));
      return true;
    }
    return false;
  }

  if (FirstPteTy.isConstQualified()) {
    // If the first parameter is a `const char *`, it is a printf/kprintf:
    bool isKprintf = false;
    const Expr *UnsafeArg;

    if (auto *II = FD->getIdentifier())
      isKprintf = II->getName() == "kprintf";
    if (hasUnsafeFormatOrSArg(&Node, UnsafeArg, 0, Ctx, isKprintf)) {
      Result.addNode(Tag, DynTypedNode::create(*UnsafeArg));
      return true;
    }
    return false;
  }

  if (NumParms > 2) {
    QualType SecondParmTy = FD->getParamDecl(1)->getType();

    if (!FirstPteTy.isConstQualified() && SecondParmTy->isIntegerType()) {
      // If the first parameter type is non-const qualified `char *` and the
      // second is an integer, it is a snprintf:
      const Expr *UnsafeArg;

      if (hasUnsafeFormatOrSArg(&Node, UnsafeArg, 2, Ctx, false)) {
        Result.addNode(Tag, DynTypedNode::create(*UnsafeArg));
        return true;
      }
      return false;
    }
  }
  // We don't really recognize this "normal" printf, the only thing we
  // can do is to require all pointers to be null-terminated:
  for (const auto *Arg : Node.arguments())
    if (Arg->getType()->isPointerType() && !isNullTermPointer(Arg)) {
      Result.addNode(Tag, DynTypedNode::create(*Arg));
      return true;
    }
  return false;
}

// This function requires that it is known that the callee `isNormalPrintf`.
// It returns true iff the first two arguments of the call is a pointer
// `Ptr` and an unsigned integer `Size` and they are NOT safe, i.e.,
// `!isPtrBufferSafe(Ptr, Size)`.
static bool hasUnsafeSnprintfBuffer(const CallExpr &Node, ASTContext &Ctx) {
  const FunctionDecl *FD = Node.getDirectCallee();

  assert(FD && "It should have been checked that FD is non-null.");

  if (FD->getNumParams() < 3)
    return false; // Not an snprint

  QualType FirstParmTy = FD->getParamDecl(0)->getType();

  if (!FirstParmTy->isPointerType())
    return false; // Not an snprint

  QualType FirstPteTy = FirstParmTy->castAs<PointerType>()->getPointeeType();
  const Expr *Buf = Node.getArg(0), *Size = Node.getArg(1);

  if (FirstPteTy.isConstQualified() || !FirstPteTy->isAnyCharacterType() ||
      !Buf->getType()->isPointerType() ||
      !Size->getType()->isUnsignedIntegerType())
    return false; // not an snprintf call

  return !isPtrBufferSafe(Buf, Size, Ctx);
}
} // namespace libc_func_matchers

namespace {
// Because the analysis revolves around variables and their types, we'll need to
// track uses of variables (aka DeclRefExprs).
using DeclUseList = SmallVector<const DeclRefExpr *, 1>;

// Convenience typedef.
using FixItList = SmallVector<FixItHint, 4>;
} // namespace

namespace {
/// Gadget is an individual operation in the code that may be of interest to
/// this analysis. Each (non-abstract) subclass corresponds to a specific
/// rigid AST structure that constitutes an operation on a pointer-type object.
/// Discovery of a gadget in the code corresponds to claiming that we understand
/// what this part of code is doing well enough to potentially improve it.
/// Gadgets can be warning (immediately deserving a warning) or fixable (not
/// always deserving a warning per se, but requires our attention to identify
/// it warrants a fixit).
class Gadget {
public:
  enum class Kind {
#define GADGET(x) x,
#include "clang/Analysis/Analyses/UnsafeBufferUsageGadgets.def"
  };

  Gadget(Kind K) : K(K) {}

  Kind getKind() const { return K; }

#ifndef NDEBUG
  StringRef getDebugName() const {
    switch (K) {
#define GADGET(x)                                                              \
  case Kind::x:                                                                \
    return #x;
#include "clang/Analysis/Analyses/UnsafeBufferUsageGadgets.def"
    }
    llvm_unreachable("Unhandled Gadget::Kind enum");
  }
#endif

  virtual bool isWarningGadget() const = 0;
  // TODO remove this method from WarningGadget interface. It's only used for
  // debug prints in FixableGadget.
  virtual SourceLocation getSourceLoc() const = 0;

  /// Returns the list of pointer-type variables on which this gadget performs
  /// its operation. Typically, there's only one variable. This isn't a list
  /// of all DeclRefExprs in the gadget's AST!
  virtual DeclUseList getClaimedVarUseSites() const = 0;

  virtual ~Gadget() = default;

private:
  Kind K;
};

/// Warning gadgets correspond to unsafe code patterns that warrants
/// an immediate warning.
class WarningGadget : public Gadget {
public:
  WarningGadget(Kind K) : Gadget(K) {}

  static bool classof(const Gadget *G) { return G->isWarningGadget(); }
  bool isWarningGadget() const final { return true; }

  virtual void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                                     bool IsRelatedToDecl,
                                     ASTContext &Ctx) const = 0;

  virtual SmallVector<const Expr *, 1> getUnsafePtrs() const = 0;
};

/// Fixable gadgets correspond to code patterns that aren't always unsafe but
/// need to be properly recognized in order to emit fixes. For example, if a raw
/// pointer-type variable is replaced by a safe C++ container, every use of such
/// variable must be carefully considered and possibly updated.
class FixableGadget : public Gadget {
public:
  FixableGadget(Kind K) : Gadget(K) {}

  static bool classof(const Gadget *G) { return !G->isWarningGadget(); }
  bool isWarningGadget() const final { return false; }

  /// Returns a fixit that would fix the current gadget according to
  /// the current strategy. Returns std::nullopt if the fix cannot be produced;
  /// returns an empty list if no fixes are necessary.
  virtual std::optional<FixItList> getFixits(const FixitStrategy &) const {
    return std::nullopt;
  }

  /// Returns a list of two elements where the first element is the LHS of a
  /// pointer assignment statement and the second element is the RHS. This
  /// two-element list represents the fact that the LHS buffer gets its bounds
  /// information from the RHS buffer. This information will be used later to
  /// group all those variables whose types must be modified together to prevent
  /// type mismatches.
  virtual std::optional<std::pair<const VarDecl *, const VarDecl *>>
  getStrategyImplications() const {
    return std::nullopt;
  }
};

static bool isSupportedVariable(const DeclRefExpr &Node) {
  const Decl *D = Node.getDecl();
  return D != nullptr && isa<VarDecl>(D);
}

using FixableGadgetList = std::vector<std::unique_ptr<FixableGadget>>;
using WarningGadgetList = std::vector<std::unique_ptr<WarningGadget>>;

/// An increment of a pointer-type value is unsafe as it may run the pointer
/// out of bounds.
class IncrementGadget : public WarningGadget {
  static constexpr const char *const OpTag = "op";
  const UnaryOperator *Op;

public:
  IncrementGadget(const MatchResult &Result)
      : WarningGadget(Kind::Increment),
        Op(Result.getNodeAs<UnaryOperator>(OpTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::Increment;
  }

  static bool matches(const Stmt *S, const ASTContext &Ctx,
                      MatchResult &Result) {
    const auto *UO = dyn_cast<UnaryOperator>(S);
    if (!UO || !UO->isIncrementOp())
      return false;
    if (!hasPointerType(*UO->getSubExpr()->IgnoreParenImpCasts()))
      return false;
    Result.addNode(OpTag, DynTypedNode::create(*UO));
    return true;
  }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeOperation(Op, IsRelatedToDecl, Ctx);
  }
  SourceLocation getSourceLoc() const override { return Op->getBeginLoc(); }

  DeclUseList getClaimedVarUseSites() const override {
    SmallVector<const DeclRefExpr *, 2> Uses;
    if (const auto *DRE =
            dyn_cast<DeclRefExpr>(Op->getSubExpr()->IgnoreParenImpCasts())) {
      Uses.push_back(DRE);
    }

    return std::move(Uses);
  }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override {
    return {Op->getSubExpr()->IgnoreParenImpCasts()};
  }
};

/// A decrement of a pointer-type value is unsafe as it may run the pointer
/// out of bounds.
class DecrementGadget : public WarningGadget {
  static constexpr const char *const OpTag = "op";
  const UnaryOperator *Op;

public:
  DecrementGadget(const MatchResult &Result)
      : WarningGadget(Kind::Decrement),
        Op(Result.getNodeAs<UnaryOperator>(OpTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::Decrement;
  }

  static bool matches(const Stmt *S, const ASTContext &Ctx,
                      MatchResult &Result) {
    const auto *UO = dyn_cast<UnaryOperator>(S);
    if (!UO || !UO->isDecrementOp())
      return false;
    if (!hasPointerType(*UO->getSubExpr()->IgnoreParenImpCasts()))
      return false;
    Result.addNode(OpTag, DynTypedNode::create(*UO));
    return true;
  }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeOperation(Op, IsRelatedToDecl, Ctx);
  }
  SourceLocation getSourceLoc() const override { return Op->getBeginLoc(); }

  DeclUseList getClaimedVarUseSites() const override {
    if (const auto *DRE =
            dyn_cast<DeclRefExpr>(Op->getSubExpr()->IgnoreParenImpCasts())) {
      return {DRE};
    }

    return {};
  }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override {
    return {Op->getSubExpr()->IgnoreParenImpCasts()};
  }
};

/// Array subscript expressions on raw pointers as if they're arrays. Unsafe as
/// it doesn't have any bounds checks for the array.
class ArraySubscriptGadget : public WarningGadget {
  static constexpr const char *const ArraySubscrTag = "ArraySubscript";
  const ArraySubscriptExpr *ASE;

public:
  ArraySubscriptGadget(const MatchResult &Result)
      : WarningGadget(Kind::ArraySubscript),
        ASE(Result.getNodeAs<ArraySubscriptExpr>(ArraySubscrTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::ArraySubscript;
  }

  static bool matches(const Stmt *S, const ASTContext &Ctx,
                      MatchResult &Result) {
    const auto *ASE = dyn_cast<ArraySubscriptExpr>(S);
    if (!ASE)
      return false;
    const auto *const Base = ASE->getBase()->IgnoreParenImpCasts();
    if (!hasPointerType(*Base) && !hasArrayType(*Base))
      return false;
    const auto *Idx = dyn_cast<IntegerLiteral>(ASE->getIdx());
    bool IsSafeIndex = (Idx && Idx->getValue().isZero()) ||
                       isa<ArrayInitIndexExpr>(ASE->getIdx());
    if (IsSafeIndex || isSafeArraySubscript(*ASE, Ctx))
      return false;
    Result.addNode(ArraySubscrTag, DynTypedNode::create(*ASE));
    return true;
  }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeOperation(ASE, IsRelatedToDecl, Ctx);
  }
  SourceLocation getSourceLoc() const override { return ASE->getBeginLoc(); }

  DeclUseList getClaimedVarUseSites() const override {
    if (const auto *DRE =
            dyn_cast<DeclRefExpr>(ASE->getBase()->IgnoreParenImpCasts())) {
      return {DRE};
    }

    return {};
  }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override {
    return {ASE->getBase()->IgnoreParenImpCasts()};
  }
};

/// A pointer arithmetic expression of one of the forms:
///  \code
///  ptr + n | n + ptr | ptr - n | ptr += n | ptr -= n
///  \endcode
class PointerArithmeticGadget : public WarningGadget {
  static constexpr const char *const PointerArithmeticTag = "ptrAdd";
  static constexpr const char *const PointerArithmeticPointerTag = "ptrAddPtr";
  const BinaryOperator *PA; // pointer arithmetic expression
  const Expr *Ptr;          // the pointer expression in `PA`

public:
  PointerArithmeticGadget(const MatchResult &Result)
      : WarningGadget(Kind::PointerArithmetic),
        PA(Result.getNodeAs<BinaryOperator>(PointerArithmeticTag)),
        Ptr(Result.getNodeAs<Expr>(PointerArithmeticPointerTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::PointerArithmetic;
  }

  static bool matches(const Stmt *S, const ASTContext &Ctx,
                      MatchResult &Result) {
    const auto *BO = dyn_cast<BinaryOperator>(S);
    if (!BO)
      return false;
    const auto *LHS = BO->getLHS();
    const auto *RHS = BO->getRHS();
    // ptr at left
    if (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub ||
        BO->getOpcode() == BO_AddAssign || BO->getOpcode() == BO_SubAssign) {
      if (hasPointerType(*LHS) && (RHS->getType()->isIntegerType() ||
                                   RHS->getType()->isEnumeralType())) {
        Result.addNode(PointerArithmeticPointerTag, DynTypedNode::create(*LHS));
        Result.addNode(PointerArithmeticTag, DynTypedNode::create(*BO));
        return true;
      }
    }
    // ptr at right
    if (BO->getOpcode() == BO_Add && hasPointerType(*RHS) &&
        (LHS->getType()->isIntegerType() || LHS->getType()->isEnumeralType())) {
      Result.addNode(PointerArithmeticPointerTag, DynTypedNode::create(*RHS));
      Result.addNode(PointerArithmeticTag, DynTypedNode::create(*BO));
      return true;
    }
    return false;
  }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeOperation(PA, IsRelatedToDecl, Ctx);
  }
  SourceLocation getSourceLoc() const override { return PA->getBeginLoc(); }

  DeclUseList getClaimedVarUseSites() const override {
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Ptr->IgnoreParenImpCasts())) {
      return {DRE};
    }

    return {};
  }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override {
    return {Ptr->IgnoreParenImpCasts()};
  }

  // FIXME: pointer adding zero should be fine
  // FIXME: this gadge will need a fix-it
};

class SpanTwoParamConstructorGadget : public WarningGadget {
  static constexpr const char *const SpanTwoParamConstructorTag =
      "spanTwoParamConstructor";
  const CXXConstructExpr *Ctor; // the span constructor expression

public:
  SpanTwoParamConstructorGadget(const MatchResult &Result)
      : WarningGadget(Kind::SpanTwoParamConstructor),
        Ctor(Result.getNodeAs<CXXConstructExpr>(SpanTwoParamConstructorTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::SpanTwoParamConstructor;
  }

  static bool matches(const Stmt *S, ASTContext &Ctx, MatchResult &Result) {
    const auto *CE = dyn_cast<CXXConstructExpr>(S);
    if (!CE)
      return false;
    const auto *CDecl = CE->getConstructor();
    const auto *CRecordDecl = CDecl->getParent();
    auto HasTwoParamSpanCtorDecl =
        CRecordDecl->isInStdNamespace() &&
        CDecl->getDeclName().getAsString() == "span" && CE->getNumArgs() == 2;
    if (!HasTwoParamSpanCtorDecl || isSafeSpanTwoParamConstruct(*CE, Ctx))
      return false;
    Result.addNode(SpanTwoParamConstructorTag, DynTypedNode::create(*CE));
    return true;
  }

  static bool matches(const Stmt *S, ASTContext &Ctx,
                      const UnsafeBufferUsageHandler *Handler,
                      MatchResult &Result) {
    if (ignoreUnsafeBufferInContainer(*S, Handler))
      return false;
    return matches(S, Ctx, Result);
  }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeOperationInContainer(Ctor, IsRelatedToDecl, Ctx);
  }
  SourceLocation getSourceLoc() const override { return Ctor->getBeginLoc(); }

  DeclUseList getClaimedVarUseSites() const override {
    // If the constructor call is of the form `std::span{var, n}`, `var` is
    // considered an unsafe variable.
    if (auto *DRE = dyn_cast<DeclRefExpr>(Ctor->getArg(0))) {
      if (isa<VarDecl>(DRE->getDecl()))
        return {DRE};
    }
    return {};
  }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override { return {}; }
};

/// A pointer initialization expression of the form:
///  \code
///  int *p = q;
///  \endcode
class PointerInitGadget : public FixableGadget {
private:
  static constexpr const char *const PointerInitLHSTag = "ptrInitLHS";
  static constexpr const char *const PointerInitRHSTag = "ptrInitRHS";
  const VarDecl *PtrInitLHS;     // the LHS pointer expression in `PI`
  const DeclRefExpr *PtrInitRHS; // the RHS pointer expression in `PI`

public:
  PointerInitGadget(const MatchResult &Result)
      : FixableGadget(Kind::PointerInit),
        PtrInitLHS(Result.getNodeAs<VarDecl>(PointerInitLHSTag)),
        PtrInitRHS(Result.getNodeAs<DeclRefExpr>(PointerInitRHSTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::PointerInit;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    const DeclStmt *DS = dyn_cast<DeclStmt>(S);
    if (!DS || !DS->isSingleDecl())
      return false;
    const VarDecl *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!VD)
      return false;
    const Expr *Init = VD->getAnyInitializer();
    if (!Init)
      return false;
    const auto *DRE = dyn_cast<DeclRefExpr>(Init->IgnoreImpCasts());
    if (!DRE || !hasPointerType(*DRE) || !isSupportedVariable(*DRE)) {
      return false;
    }
    MatchResult R;
    R.addNode(PointerInitLHSTag, DynTypedNode::create(*VD));
    R.addNode(PointerInitRHSTag, DynTypedNode::create(*DRE));
    Results.emplace_back(std::move(R));
    return true;
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &S) const override;
  SourceLocation getSourceLoc() const override {
    return PtrInitRHS->getBeginLoc();
  }

  virtual DeclUseList getClaimedVarUseSites() const override {
    return DeclUseList{PtrInitRHS};
  }

  virtual std::optional<std::pair<const VarDecl *, const VarDecl *>>
  getStrategyImplications() const override {
    return std::make_pair(PtrInitLHS, cast<VarDecl>(PtrInitRHS->getDecl()));
  }
};

/// A pointer assignment expression of the form:
///  \code
///  p = q;
///  \endcode
/// where both `p` and `q` are pointers.
class PtrToPtrAssignmentGadget : public FixableGadget {
private:
  static constexpr const char *const PointerAssignLHSTag = "ptrLHS";
  static constexpr const char *const PointerAssignRHSTag = "ptrRHS";
  const DeclRefExpr *PtrLHS; // the LHS pointer expression in `PA`
  const DeclRefExpr *PtrRHS; // the RHS pointer expression in `PA`

public:
  PtrToPtrAssignmentGadget(const MatchResult &Result)
      : FixableGadget(Kind::PtrToPtrAssignment),
        PtrLHS(Result.getNodeAs<DeclRefExpr>(PointerAssignLHSTag)),
        PtrRHS(Result.getNodeAs<DeclRefExpr>(PointerAssignRHSTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::PtrToPtrAssignment;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    size_t SizeBefore = Results.size();
    findStmtsInUnspecifiedUntypedContext(S, [&Results](const Stmt *S) {
      const auto *BO = dyn_cast<BinaryOperator>(S);
      if (!BO || BO->getOpcode() != BO_Assign)
        return;
      const auto *RHS = BO->getRHS()->IgnoreParenImpCasts();
      if (const auto *RHSRef = dyn_cast<DeclRefExpr>(RHS);
          !RHSRef || !hasPointerType(*RHSRef) ||
          !isSupportedVariable(*RHSRef)) {
        return;
      }
      const auto *LHS = BO->getLHS();
      if (const auto *LHSRef = dyn_cast<DeclRefExpr>(LHS);
          !LHSRef || !hasPointerType(*LHSRef) ||
          !isSupportedVariable(*LHSRef)) {
        return;
      }
      MatchResult R;
      R.addNode(PointerAssignLHSTag, DynTypedNode::create(*LHS));
      R.addNode(PointerAssignRHSTag, DynTypedNode::create(*RHS));
      Results.emplace_back(std::move(R));
    });
    return SizeBefore != Results.size();
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &S) const override;
  SourceLocation getSourceLoc() const override { return PtrLHS->getBeginLoc(); }

  virtual DeclUseList getClaimedVarUseSites() const override {
    return DeclUseList{PtrLHS, PtrRHS};
  }

  virtual std::optional<std::pair<const VarDecl *, const VarDecl *>>
  getStrategyImplications() const override {
    return std::make_pair(cast<VarDecl>(PtrLHS->getDecl()),
                          cast<VarDecl>(PtrRHS->getDecl()));
  }
};

/// An assignment expression of the form:
///  \code
///  ptr = array;
///  \endcode
/// where `p` is a pointer and `array` is a constant size array.
class CArrayToPtrAssignmentGadget : public FixableGadget {
private:
  static constexpr const char *const PointerAssignLHSTag = "ptrLHS";
  static constexpr const char *const PointerAssignRHSTag = "ptrRHS";
  const DeclRefExpr *PtrLHS; // the LHS pointer expression in `PA`
  const DeclRefExpr *PtrRHS; // the RHS pointer expression in `PA`

public:
  CArrayToPtrAssignmentGadget(const MatchResult &Result)
      : FixableGadget(Kind::CArrayToPtrAssignment),
        PtrLHS(Result.getNodeAs<DeclRefExpr>(PointerAssignLHSTag)),
        PtrRHS(Result.getNodeAs<DeclRefExpr>(PointerAssignRHSTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::CArrayToPtrAssignment;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    size_t SizeBefore = Results.size();
    findStmtsInUnspecifiedUntypedContext(S, [&Results](const Stmt *S) {
      const auto *BO = dyn_cast<BinaryOperator>(S);
      if (!BO || BO->getOpcode() != BO_Assign)
        return;
      const auto *RHS = BO->getRHS()->IgnoreParenImpCasts();
      if (const auto *RHSRef = dyn_cast<DeclRefExpr>(RHS);
          !RHSRef ||
          !isa<ConstantArrayType>(RHSRef->getType().getCanonicalType()) ||
          !isSupportedVariable(*RHSRef)) {
        return;
      }
      const auto *LHS = BO->getLHS();
      if (const auto *LHSRef = dyn_cast<DeclRefExpr>(LHS);
          !LHSRef || !hasPointerType(*LHSRef) ||
          !isSupportedVariable(*LHSRef)) {
        return;
      }
      MatchResult R;
      R.addNode(PointerAssignLHSTag, DynTypedNode::create(*LHS));
      R.addNode(PointerAssignRHSTag, DynTypedNode::create(*RHS));
      Results.emplace_back(std::move(R));
    });
    return SizeBefore != Results.size();
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &S) const override;
  SourceLocation getSourceLoc() const override { return PtrLHS->getBeginLoc(); }

  virtual DeclUseList getClaimedVarUseSites() const override {
    return DeclUseList{PtrLHS, PtrRHS};
  }

  virtual std::optional<std::pair<const VarDecl *, const VarDecl *>>
  getStrategyImplications() const override {
    return {};
  }
};

/// A call of a function or method that performs unchecked buffer operations
/// over one of its pointer parameters.
class UnsafeBufferUsageAttrGadget : public WarningGadget {
  constexpr static const char *const OpTag = "attr_expr";
  const Expr *Op;

public:
  UnsafeBufferUsageAttrGadget(const MatchResult &Result)
      : WarningGadget(Kind::UnsafeBufferUsageAttr),
        Op(Result.getNodeAs<Expr>(OpTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::UnsafeBufferUsageAttr;
  }

  static bool matches(const Stmt *S, const ASTContext &Ctx,
                      MatchResult &Result) {
    if (auto *CE = dyn_cast<CallExpr>(S)) {
      if (CE->getDirectCallee() &&
          CE->getDirectCallee()->hasAttr<UnsafeBufferUsageAttr>()) {
        Result.addNode(OpTag, DynTypedNode::create(*CE));
        return true;
      }
    }
    if (auto *ME = dyn_cast<MemberExpr>(S)) {
      if (!isa<FieldDecl>(ME->getMemberDecl()))
        return false;
      if (ME->getMemberDecl()->hasAttr<UnsafeBufferUsageAttr>()) {
        Result.addNode(OpTag, DynTypedNode::create(*ME));
        return true;
      }
    }
    return false;
  }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeOperation(Op, IsRelatedToDecl, Ctx);
  }
  SourceLocation getSourceLoc() const override { return Op->getBeginLoc(); }

  DeclUseList getClaimedVarUseSites() const override { return {}; }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override { return {}; }
};

/// A call of a constructor that performs unchecked buffer operations
/// over one of its pointer parameters, or constructs a class object that will
/// perform buffer operations that depend on the correctness of the parameters.
class UnsafeBufferUsageCtorAttrGadget : public WarningGadget {
  constexpr static const char *const OpTag = "cxx_construct_expr";
  const CXXConstructExpr *Op;

public:
  UnsafeBufferUsageCtorAttrGadget(const MatchResult &Result)
      : WarningGadget(Kind::UnsafeBufferUsageCtorAttr),
        Op(Result.getNodeAs<CXXConstructExpr>(OpTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::UnsafeBufferUsageCtorAttr;
  }

  static bool matches(const Stmt *S, ASTContext &Ctx, MatchResult &Result) {
    const auto *CE = dyn_cast<CXXConstructExpr>(S);
    if (!CE || !CE->getConstructor()->hasAttr<UnsafeBufferUsageAttr>())
      return false;
    // std::span(ptr, size) ctor is handled by SpanTwoParamConstructorGadget.
    MatchResult Tmp;
    if (SpanTwoParamConstructorGadget::matches(CE, Ctx, Tmp))
      return false;
    Result.addNode(OpTag, DynTypedNode::create(*CE));
    return true;
  }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeOperation(Op, IsRelatedToDecl, Ctx);
  }
  SourceLocation getSourceLoc() const override { return Op->getBeginLoc(); }

  DeclUseList getClaimedVarUseSites() const override { return {}; }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override { return {}; }
};

// Warning gadget for unsafe invocation of span::data method.
// Triggers when the pointer returned by the invocation is immediately
// cast to a larger type.

class DataInvocationGadget : public WarningGadget {
  constexpr static const char *const OpTag = "data_invocation_expr";
  const ExplicitCastExpr *Op;

public:
  DataInvocationGadget(const MatchResult &Result)
      : WarningGadget(Kind::DataInvocation),
        Op(Result.getNodeAs<ExplicitCastExpr>(OpTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::DataInvocation;
  }

  static bool matches(const Stmt *S, const ASTContext &Ctx,
                      MatchResult &Result) {
    auto *CE = dyn_cast<ExplicitCastExpr>(S);
    if (!CE)
      return false;
    for (auto *Child : CE->children()) {
      if (auto *MCE = dyn_cast<CXXMemberCallExpr>(Child);
          MCE && isDataFunction(MCE)) {
        Result.addNode(OpTag, DynTypedNode::create(*CE));
        return true;
      }
      if (auto *Paren = dyn_cast<ParenExpr>(Child)) {
        if (auto *MCE = dyn_cast<CXXMemberCallExpr>(Paren->getSubExpr());
            MCE && isDataFunction(MCE)) {
          Result.addNode(OpTag, DynTypedNode::create(*CE));
          return true;
        }
      }
    }
    return false;
  }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeOperation(Op, IsRelatedToDecl, Ctx);
  }
  SourceLocation getSourceLoc() const override { return Op->getBeginLoc(); }

  DeclUseList getClaimedVarUseSites() const override { return {}; }

private:
  static bool isDataFunction(const CXXMemberCallExpr *call) {
    if (!call)
      return false;
    auto *callee = call->getDirectCallee();
    if (!callee || !isa<CXXMethodDecl>(callee))
      return false;
    auto *method = cast<CXXMethodDecl>(callee);
    if (method->getNameAsString() == "data" &&
        method->getParent()->isInStdNamespace() &&
        llvm::is_contained({SIZED_CONTAINER_OR_VIEW_LIST},
                           method->getParent()->getName()))
      return true;
    return false;
  }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override { return {}; }
};

class UnsafeLibcFunctionCallGadget : public WarningGadget {
  const CallExpr *const Call;
  const Expr *UnsafeArg = nullptr;
  constexpr static const char *const Tag = "UnsafeLibcFunctionCall";
  // Extra tags for additional information:
  constexpr static const char *const UnsafeSprintfTag =
      "UnsafeLibcFunctionCall_sprintf";
  constexpr static const char *const UnsafeSizedByTag =
      "UnsafeLibcFunctionCall_sized_by";
  constexpr static const char *const UnsafeStringTag =
      "UnsafeLibcFunctionCall_string";
  constexpr static const char *const UnsafeVaListTag =
      "UnsafeLibcFunctionCall_va_list";

  enum UnsafeKind {
    OTHERS = 0,  // no specific information, the callee function is unsafe
    SPRINTF = 1, // never call `-sprintf`s, call `-snprintf`s instead.
    SIZED_BY =
        2, // the first two arguments of `snprintf` function have
           // "__sized_by" relation but they do not conform to safe patterns
    STRING = 3,  // an argument is a pointer-to-char-as-string but does not
                 // guarantee null-termination
    VA_LIST = 4, // one of the `-printf`s function that take va_list, which is
                 // considered unsafe as it is not compile-time check
  } WarnedFunKind = OTHERS;

public:
  UnsafeLibcFunctionCallGadget(const MatchResult &Result)
      : WarningGadget(Kind::UnsafeLibcFunctionCall),
        Call(Result.getNodeAs<CallExpr>(Tag)) {
    if (Result.getNodeAs<Decl>(UnsafeSprintfTag))
      WarnedFunKind = SPRINTF;
    else if (auto *E = Result.getNodeAs<Expr>(UnsafeStringTag)) {
      WarnedFunKind = STRING;
      UnsafeArg = E;
    } else if (Result.getNodeAs<CallExpr>(UnsafeSizedByTag)) {
      WarnedFunKind = SIZED_BY;
      UnsafeArg = Call->getArg(0);
    } else if (Result.getNodeAs<Decl>(UnsafeVaListTag))
      WarnedFunKind = VA_LIST;
  }

  static bool matches(const Stmt *S, ASTContext &Ctx,
                      const UnsafeBufferUsageHandler *Handler,
                      MatchResult &Result) {
    if (ignoreUnsafeLibcCall(Ctx, *S, Handler))
      return false;
    auto *CE = dyn_cast<CallExpr>(S);
    if (!CE || !CE->getDirectCallee())
      return false;
    const auto *FD = dyn_cast<FunctionDecl>(CE->getDirectCallee());
    if (!FD)
      return false;
    auto isSingleStringLiteralArg = false;
    if (CE->getNumArgs() == 1) {
      isSingleStringLiteralArg =
          isa<clang::StringLiteral>(CE->getArg(0)->IgnoreParenImpCasts());
    }
    if (!isSingleStringLiteralArg) {
      // (unless the call has a sole string literal argument):
      if (libc_func_matchers::isPredefinedUnsafeLibcFunc(*FD)) {
        Result.addNode(Tag, DynTypedNode::create(*CE));
        return true;
      }
      if (libc_func_matchers::isUnsafeVaListPrintfFunc(*FD)) {
        Result.addNode(Tag, DynTypedNode::create(*CE));
        Result.addNode(UnsafeVaListTag, DynTypedNode::create(*FD));
        return true;
      }
      if (libc_func_matchers::isUnsafeSprintfFunc(*FD)) {
        Result.addNode(Tag, DynTypedNode::create(*CE));
        Result.addNode(UnsafeSprintfTag, DynTypedNode::create(*FD));
        return true;
      }
    }
    if (libc_func_matchers::isNormalPrintfFunc(*FD)) {
      if (libc_func_matchers::hasUnsafeSnprintfBuffer(*CE, Ctx)) {
        Result.addNode(Tag, DynTypedNode::create(*CE));
        Result.addNode(UnsafeSizedByTag, DynTypedNode::create(*CE));
        return true;
      }
      if (libc_func_matchers::hasUnsafePrintfStringArg(*CE, Ctx, Result,
                                                       UnsafeStringTag)) {
        Result.addNode(Tag, DynTypedNode::create(*CE));
        return true;
      }
    }
    return false;
  }

  const Stmt *getBaseStmt() const { return Call; }

  SourceLocation getSourceLoc() const override { return Call->getBeginLoc(); }

  void handleUnsafeOperation(UnsafeBufferUsageHandler &Handler,
                             bool IsRelatedToDecl,
                             ASTContext &Ctx) const override {
    Handler.handleUnsafeLibcCall(Call, WarnedFunKind, Ctx, UnsafeArg);
  }

  DeclUseList getClaimedVarUseSites() const override { return {}; }

  SmallVector<const Expr *, 1> getUnsafePtrs() const override { return {}; }
};

// Represents expressions of the form `DRE[*]` in the Unspecified Lvalue
// Context (see `findStmtsInUnspecifiedLvalueContext`).
// Note here `[]` is the built-in subscript operator.
class ULCArraySubscriptGadget : public FixableGadget {
private:
  static constexpr const char *const ULCArraySubscriptTag =
      "ArraySubscriptUnderULC";
  const ArraySubscriptExpr *Node;

public:
  ULCArraySubscriptGadget(const MatchResult &Result)
      : FixableGadget(Kind::ULCArraySubscript),
        Node(Result.getNodeAs<ArraySubscriptExpr>(ULCArraySubscriptTag)) {
    assert(Node != nullptr && "Expecting a non-null matching result");
  }

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::ULCArraySubscript;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    size_t SizeBefore = Results.size();
    findStmtsInUnspecifiedLvalueContext(S, [&Results](const Expr *E) {
      const auto *ASE = dyn_cast<ArraySubscriptExpr>(E);
      if (!ASE)
        return;
      const auto *DRE =
          dyn_cast<DeclRefExpr>(ASE->getBase()->IgnoreParenImpCasts());
      if (!DRE || !(hasPointerType(*DRE) || hasArrayType(*DRE)) ||
          !isSupportedVariable(*DRE))
        return;
      MatchResult R;
      R.addNode(ULCArraySubscriptTag, DynTypedNode::create(*ASE));
      Results.emplace_back(std::move(R));
    });
    return SizeBefore != Results.size();
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &S) const override;
  SourceLocation getSourceLoc() const override { return Node->getBeginLoc(); }

  virtual DeclUseList getClaimedVarUseSites() const override {
    if (const auto *DRE =
            dyn_cast<DeclRefExpr>(Node->getBase()->IgnoreImpCasts())) {
      return {DRE};
    }
    return {};
  }
};

// Fixable gadget to handle stand alone pointers of the form `UPC(DRE)` in the
// unspecified pointer context (findStmtsInUnspecifiedPointerContext). The
// gadget emits fixit of the form `UPC(DRE.data())`.
class UPCStandalonePointerGadget : public FixableGadget {
private:
  static constexpr const char *const DeclRefExprTag = "StandalonePointer";
  const DeclRefExpr *Node;

public:
  UPCStandalonePointerGadget(const MatchResult &Result)
      : FixableGadget(Kind::UPCStandalonePointer),
        Node(Result.getNodeAs<DeclRefExpr>(DeclRefExprTag)) {
    assert(Node != nullptr && "Expecting a non-null matching result");
  }

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::UPCStandalonePointer;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    size_t SizeBefore = Results.size();
    findStmtsInUnspecifiedPointerContext(S, [&Results](const Stmt *S) {
      auto *E = dyn_cast<Expr>(S);
      if (!E)
        return;
      const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
      if (!DRE || (!hasPointerType(*DRE) && !hasArrayType(*DRE)) ||
          !isSupportedVariable(*DRE))
        return;
      MatchResult R;
      R.addNode(DeclRefExprTag, DynTypedNode::create(*DRE));
      Results.emplace_back(std::move(R));
    });
    return SizeBefore != Results.size();
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &S) const override;
  SourceLocation getSourceLoc() const override { return Node->getBeginLoc(); }

  virtual DeclUseList getClaimedVarUseSites() const override { return {Node}; }
};

class PointerDereferenceGadget : public FixableGadget {
  static constexpr const char *const BaseDeclRefExprTag = "BaseDRE";
  static constexpr const char *const OperatorTag = "op";

  const DeclRefExpr *BaseDeclRefExpr = nullptr;
  const UnaryOperator *Op = nullptr;

public:
  PointerDereferenceGadget(const MatchResult &Result)
      : FixableGadget(Kind::PointerDereference),
        BaseDeclRefExpr(Result.getNodeAs<DeclRefExpr>(BaseDeclRefExprTag)),
        Op(Result.getNodeAs<UnaryOperator>(OperatorTag)) {}

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::PointerDereference;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    size_t SizeBefore = Results.size();
    findStmtsInUnspecifiedLvalueContext(S, [&Results](const Stmt *S) {
      const auto *UO = dyn_cast<UnaryOperator>(S);
      if (!UO || UO->getOpcode() != UO_Deref)
        return;
      const auto *CE = dyn_cast<Expr>(UO->getSubExpr());
      if (!CE)
        return;
      CE = CE->IgnoreParenImpCasts();
      const auto *DRE = dyn_cast<DeclRefExpr>(CE);
      if (!DRE || !isSupportedVariable(*DRE))
        return;
      MatchResult R;
      R.addNode(BaseDeclRefExprTag, DynTypedNode::create(*DRE));
      R.addNode(OperatorTag, DynTypedNode::create(*UO));
      Results.emplace_back(std::move(R));
    });
    return SizeBefore != Results.size();
  }

  DeclUseList getClaimedVarUseSites() const override {
    return {BaseDeclRefExpr};
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &S) const override;
  SourceLocation getSourceLoc() const override { return Op->getBeginLoc(); }
};

// Represents expressions of the form `&DRE[any]` in the Unspecified Pointer
// Context (see `findStmtsInUnspecifiedPointerContext`).
// Note here `[]` is the built-in subscript operator.
class UPCAddressofArraySubscriptGadget : public FixableGadget {
private:
  static constexpr const char *const UPCAddressofArraySubscriptTag =
      "AddressofArraySubscriptUnderUPC";
  const UnaryOperator *Node; // the `&DRE[any]` node

public:
  UPCAddressofArraySubscriptGadget(const MatchResult &Result)
      : FixableGadget(Kind::ULCArraySubscript),
        Node(Result.getNodeAs<UnaryOperator>(UPCAddressofArraySubscriptTag)) {
    assert(Node != nullptr && "Expecting a non-null matching result");
  }

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::UPCAddressofArraySubscript;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    size_t SizeBefore = Results.size();
    findStmtsInUnspecifiedPointerContext(S, [&Results](const Stmt *S) {
      auto *E = dyn_cast<Expr>(S);
      if (!E)
        return;
      const auto *UO = dyn_cast<UnaryOperator>(E->IgnoreImpCasts());
      if (!UO || UO->getOpcode() != UO_AddrOf)
        return;
      const auto *ASE = dyn_cast<ArraySubscriptExpr>(UO->getSubExpr());
      if (!ASE)
        return;
      const auto *DRE =
          dyn_cast<DeclRefExpr>(ASE->getBase()->IgnoreParenImpCasts());
      if (!DRE || !isSupportedVariable(*DRE))
        return;
      MatchResult R;
      R.addNode(UPCAddressofArraySubscriptTag, DynTypedNode::create(*UO));
      Results.emplace_back(std::move(R));
    });
    return SizeBefore != Results.size();
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &) const override;
  SourceLocation getSourceLoc() const override { return Node->getBeginLoc(); }

  virtual DeclUseList getClaimedVarUseSites() const override {
    const auto *ArraySubst = cast<ArraySubscriptExpr>(Node->getSubExpr());
    const auto *DRE =
        cast<DeclRefExpr>(ArraySubst->getBase()->IgnoreParenImpCasts());
    return {DRE};
  }
};
} // namespace

namespace {
// An auxiliary tracking facility for the fixit analysis. It helps connect
// declarations to its uses and make sure we've covered all uses with our
// analysis before we try to fix the declaration.
class DeclUseTracker {
  using UseSetTy = llvm::SmallSet<const DeclRefExpr *, 16>;
  using DefMapTy = llvm::DenseMap<const VarDecl *, const DeclStmt *>;

  // Allocate on the heap for easier move.
  std::unique_ptr<UseSetTy> Uses{std::make_unique<UseSetTy>()};
  DefMapTy Defs{};

public:
  DeclUseTracker() = default;
  DeclUseTracker(const DeclUseTracker &) = delete; // Let's avoid copies.
  DeclUseTracker &operator=(const DeclUseTracker &) = delete;
  DeclUseTracker(DeclUseTracker &&) = default;
  DeclUseTracker &operator=(DeclUseTracker &&) = default;

  // Start tracking a freshly discovered DRE.
  void discoverUse(const DeclRefExpr *DRE) { Uses->insert(DRE); }

  // Stop tracking the DRE as it's been fully figured out.
  void claimUse(const DeclRefExpr *DRE) {
    assert(Uses->count(DRE) &&
           "DRE not found or claimed by multiple matchers!");
    Uses->erase(DRE);
  }

  // A variable is unclaimed if at least one use is unclaimed.
  bool hasUnclaimedUses(const VarDecl *VD) const {
    // FIXME: Can this be less linear? Maybe maintain a map from VDs to DREs?
    return any_of(*Uses, [VD](const DeclRefExpr *DRE) {
      return DRE->getDecl()->getCanonicalDecl() == VD->getCanonicalDecl();
    });
  }

  UseSetTy getUnclaimedUses(const VarDecl *VD) const {
    UseSetTy ReturnSet;
    for (auto use : *Uses) {
      if (use->getDecl()->getCanonicalDecl() == VD->getCanonicalDecl()) {
        ReturnSet.insert(use);
      }
    }
    return ReturnSet;
  }

  void discoverDecl(const DeclStmt *DS) {
    for (const Decl *D : DS->decls()) {
      if (const auto *VD = dyn_cast<VarDecl>(D)) {
        // FIXME: Assertion temporarily disabled due to a bug in
        // ASTMatcher internal behavior in presence of GNU
        // statement-expressions. We need to properly investigate this
        // because it can screw up our algorithm in other ways.
        // assert(Defs.count(VD) == 0 && "Definition already discovered!");
        Defs[VD] = DS;
      }
    }
  }

  const DeclStmt *lookupDecl(const VarDecl *VD) const {
    return Defs.lookup(VD);
  }
};
} // namespace

// Representing a pointer type expression of the form `++Ptr` in an Unspecified
// Pointer Context (UPC):
class UPCPreIncrementGadget : public FixableGadget {
private:
  static constexpr const char *const UPCPreIncrementTag =
      "PointerPreIncrementUnderUPC";
  const UnaryOperator *Node; // the `++Ptr` node

public:
  UPCPreIncrementGadget(const MatchResult &Result)
      : FixableGadget(Kind::UPCPreIncrement),
        Node(Result.getNodeAs<UnaryOperator>(UPCPreIncrementTag)) {
    assert(Node != nullptr && "Expecting a non-null matching result");
  }

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::UPCPreIncrement;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    // Note here we match `++Ptr` for any expression `Ptr` of pointer type.
    // Although currently we can only provide fix-its when `Ptr` is a DRE, we
    // can have the matcher be general, so long as `getClaimedVarUseSites` does
    // things right.
    size_t SizeBefore = Results.size();
    findStmtsInUnspecifiedPointerContext(S, [&Results](const Stmt *S) {
      auto *E = dyn_cast<Expr>(S);
      if (!E)
        return;
      const auto *UO = dyn_cast<UnaryOperator>(E->IgnoreImpCasts());
      if (!UO || UO->getOpcode() != UO_PreInc)
        return;
      const auto *DRE = dyn_cast<DeclRefExpr>(UO->getSubExpr());
      if (!DRE || !isSupportedVariable(*DRE))
        return;
      MatchResult R;
      R.addNode(UPCPreIncrementTag, DynTypedNode::create(*UO));
      Results.emplace_back(std::move(R));
    });
    return SizeBefore != Results.size();
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &S) const override;
  SourceLocation getSourceLoc() const override { return Node->getBeginLoc(); }

  virtual DeclUseList getClaimedVarUseSites() const override {
    return {dyn_cast<DeclRefExpr>(Node->getSubExpr())};
  }
};

// Representing a pointer type expression of the form `Ptr += n` in an
// Unspecified Untyped Context (UUC):
class UUCAddAssignGadget : public FixableGadget {
private:
  static constexpr const char *const UUCAddAssignTag =
      "PointerAddAssignUnderUUC";
  static constexpr const char *const OffsetTag = "Offset";

  const BinaryOperator *Node; // the `Ptr += n` node
  const Expr *Offset = nullptr;

public:
  UUCAddAssignGadget(const MatchResult &Result)
      : FixableGadget(Kind::UUCAddAssign),
        Node(Result.getNodeAs<BinaryOperator>(UUCAddAssignTag)),
        Offset(Result.getNodeAs<Expr>(OffsetTag)) {
    assert(Node != nullptr && "Expecting a non-null matching result");
  }

  static bool classof(const Gadget *G) {
    return G->getKind() == Kind::UUCAddAssign;
  }

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    size_t SizeBefore = Results.size();
    findStmtsInUnspecifiedUntypedContext(S, [&Results](const Stmt *S) {
      const auto *E = dyn_cast<Expr>(S);
      if (!E)
        return;
      const auto *BO = dyn_cast<BinaryOperator>(E->IgnoreImpCasts());
      if (!BO || BO->getOpcode() != BO_AddAssign)
        return;
      const auto *DRE = dyn_cast<DeclRefExpr>(BO->getLHS());
      if (!DRE || !hasPointerType(*DRE) || !isSupportedVariable(*DRE))
        return;
      MatchResult R;
      R.addNode(UUCAddAssignTag, DynTypedNode::create(*BO));
      R.addNode(OffsetTag, DynTypedNode::create(*BO->getRHS()));
      Results.emplace_back(std::move(R));
    });
    return SizeBefore != Results.size();
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &S) const override;
  SourceLocation getSourceLoc() const override { return Node->getBeginLoc(); }

  virtual DeclUseList getClaimedVarUseSites() const override {
    return {dyn_cast<DeclRefExpr>(Node->getLHS())};
  }
};

// Representing a fixable expression of the form `*(ptr + 123)` or `*(123 +
// ptr)`:
class DerefSimplePtrArithFixableGadget : public FixableGadget {
  static constexpr const char *const BaseDeclRefExprTag = "BaseDRE";
  static constexpr const char *const DerefOpTag = "DerefOp";
  static constexpr const char *const AddOpTag = "AddOp";
  static constexpr const char *const OffsetTag = "Offset";

  const DeclRefExpr *BaseDeclRefExpr = nullptr;
  const UnaryOperator *DerefOp = nullptr;
  const BinaryOperator *AddOp = nullptr;
  const IntegerLiteral *Offset = nullptr;

public:
  DerefSimplePtrArithFixableGadget(const MatchResult &Result)
      : FixableGadget(Kind::DerefSimplePtrArithFixable),
        BaseDeclRefExpr(Result.getNodeAs<DeclRefExpr>(BaseDeclRefExprTag)),
        DerefOp(Result.getNodeAs<UnaryOperator>(DerefOpTag)),
        AddOp(Result.getNodeAs<BinaryOperator>(AddOpTag)),
        Offset(Result.getNodeAs<IntegerLiteral>(OffsetTag)) {}

  static bool matches(const Stmt *S,
                      llvm::SmallVectorImpl<MatchResult> &Results) {
    auto IsPtr = [](const Expr *E, MatchResult &R) {
      if (!E || !hasPointerType(*E))
        return false;
      const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreImpCasts());
      if (!DRE || !isSupportedVariable(*DRE))
        return false;
      R.addNode(BaseDeclRefExprTag, DynTypedNode::create(*DRE));
      return true;
    };
    const auto IsPlusOverPtrAndInteger = [&IsPtr](const Expr *E,
                                                  MatchResult &R) {
      const auto *BO = dyn_cast<BinaryOperator>(E);
      if (!BO || BO->getOpcode() != BO_Add)
        return false;

      const auto *LHS = BO->getLHS();
      const auto *RHS = BO->getRHS();
      if (isa<IntegerLiteral>(RHS) && IsPtr(LHS, R)) {
        R.addNode(OffsetTag, DynTypedNode::create(*RHS));
        R.addNode(AddOpTag, DynTypedNode::create(*BO));
        return true;
      }
      if (isa<IntegerLiteral>(LHS) && IsPtr(RHS, R)) {
        R.addNode(OffsetTag, DynTypedNode::create(*LHS));
        R.addNode(AddOpTag, DynTypedNode::create(*BO));
        return true;
      }
      return false;
    };
    size_t SizeBefore = Results.size();
    const auto InnerMatcher = [&IsPlusOverPtrAndInteger,
                               &Results](const Expr *E) {
      const auto *UO = dyn_cast<UnaryOperator>(E);
      if (!UO || UO->getOpcode() != UO_Deref)
        return;

      const auto *Operand = UO->getSubExpr()->IgnoreParens();
      MatchResult R;
      if (IsPlusOverPtrAndInteger(Operand, R)) {
        R.addNode(DerefOpTag, DynTypedNode::create(*UO));
        Results.emplace_back(std::move(R));
      }
    };
    findStmtsInUnspecifiedLvalueContext(S, InnerMatcher);
    return SizeBefore != Results.size();
  }

  virtual std::optional<FixItList>
  getFixits(const FixitStrategy &s) const final;
  SourceLocation getSourceLoc() const override {
    return DerefOp->getBeginLoc();
  }

  virtual DeclUseList getClaimedVarUseSites() const final {
    return {BaseDeclRefExpr};
  }
};

class WarningGadgetMatcher : public FastMatcher {

public:
  WarningGadgetMatcher(WarningGadgetList &WarningGadgets)
      : WarningGadgets(WarningGadgets) {}

  bool matches(const DynTypedNode &DynNode, ASTContext &Ctx,
               const UnsafeBufferUsageHandler &Handler) override {
    const Stmt *S = DynNode.get<Stmt>();
    if (!S)
      return false;

    MatchResult Result;
#define WARNING_GADGET(name)                                                   \
  if (name##Gadget::matches(S, Ctx, Result) &&                                 \
      notInSafeBufferOptOut(*S, &Handler)) {                                   \
    WarningGadgets.push_back(std::make_unique<name##Gadget>(Result));          \
    return true;                                                               \
  }
#define WARNING_OPTIONAL_GADGET(name)                                          \
  if (name##Gadget::matches(S, Ctx, &Handler, Result) &&                       \
      notInSafeBufferOptOut(*S, &Handler)) {                                   \
    WarningGadgets.push_back(std::make_unique<name##Gadget>(Result));          \
    return true;                                                               \
  }
#include "clang/Analysis/Analyses/UnsafeBufferUsageGadgets.def"
    return false;
  }

private:
  WarningGadgetList &WarningGadgets;
};

class FixableGadgetMatcher : public FastMatcher {

public:
  FixableGadgetMatcher(FixableGadgetList &FixableGadgets,
                       DeclUseTracker &Tracker)
      : FixableGadgets(FixableGadgets), Tracker(Tracker) {}

  bool matches(const DynTypedNode &DynNode, ASTContext &Ctx,
               const UnsafeBufferUsageHandler &Handler) override {
    bool matchFound = false;
    const Stmt *S = DynNode.get<Stmt>();
    if (!S) {
      return matchFound;
    }

    llvm::SmallVector<MatchResult> Results;
#define FIXABLE_GADGET(name)                                                   \
  if (name##Gadget::matches(S, Results)) {                                     \
    for (const auto &R : Results) {                                            \
      FixableGadgets.push_back(std::make_unique<name##Gadget>(R));             \
      matchFound = true;                                                       \
    }                                                                          \
    Results = {};                                                              \
  }
#include "clang/Analysis/Analyses/UnsafeBufferUsageGadgets.def"
    // In parallel, match all DeclRefExprs so that to find out
    // whether there are any uncovered by gadgets.
    if (auto *DRE = findDeclRefExpr(S); DRE) {
      Tracker.discoverUse(DRE);
      matchFound = true;
    }
    // Also match DeclStmts because we'll need them when fixing
    // their underlying VarDecls that otherwise don't have
    // any backreferences to DeclStmts.
    if (auto *DS = findDeclStmt(S); DS) {
      Tracker.discoverDecl(DS);
      matchFound = true;
    }
    return matchFound;
  }

private:
  const DeclRefExpr *findDeclRefExpr(const Stmt *S) {
    const auto *DRE = dyn_cast<DeclRefExpr>(S);
    if (!DRE || (!hasPointerType(*DRE) && !hasArrayType(*DRE)))
      return nullptr;
    const Decl *D = DRE->getDecl();
    if (!D || (!isa<VarDecl>(D) && !isa<BindingDecl>(D)))
      return nullptr;
    return DRE;
  }
  const DeclStmt *findDeclStmt(const Stmt *S) {
    const auto *DS = dyn_cast<DeclStmt>(S);
    if (!DS)
      return nullptr;
    return DS;
  }
  FixableGadgetList &FixableGadgets;
  DeclUseTracker &Tracker;
};

// Scan the function and return a list of gadgets found with provided kits.
static void findGadgets(const Stmt *S, ASTContext &Ctx,
                        const UnsafeBufferUsageHandler &Handler,
                        bool EmitSuggestions, FixableGadgetList &FixableGadgets,
                        WarningGadgetList &WarningGadgets,
                        DeclUseTracker &Tracker) {
  WarningGadgetMatcher WMatcher{WarningGadgets};
  forEachDescendantEvaluatedStmt(S, Ctx, Handler, WMatcher);
  if (EmitSuggestions) {
    FixableGadgetMatcher FMatcher{FixableGadgets, Tracker};
    forEachDescendantStmt(S, Ctx, Handler, FMatcher);
  }
}

// Compares AST nodes by source locations.
template <typename NodeTy> struct CompareNode {
  bool operator()(const NodeTy *N1, const NodeTy *N2) const {
    return N1->getBeginLoc().getRawEncoding() <
           N2->getBeginLoc().getRawEncoding();
  }
};

std::set<const Expr *> clang::findUnsafePointers(const FunctionDecl *FD) {
  class MockReporter : public UnsafeBufferUsageHandler {
  public:
    MockReporter() {}
    void handleUnsafeOperation(const Stmt *, bool, ASTContext &) override {}
    void handleUnsafeLibcCall(const CallExpr *, unsigned, ASTContext &,
                              const Expr *UnsafeArg = nullptr) override {}
    void handleUnsafeOperationInContainer(const Stmt *, bool,
                                          ASTContext &) override {}
    void handleUnsafeVariableGroup(const VarDecl *,
                                   const VariableGroupsManager &, FixItList &&,
                                   const Decl *,
                                   const FixitStrategy &) override {}
    bool isSafeBufferOptOut(const SourceLocation &) const override {
      return false;
    }
    bool ignoreUnsafeBufferInContainer(const SourceLocation &) const override {
      return false;
    }
    bool ignoreUnsafeBufferInLibcCall(const SourceLocation &) const override {
      return false;
    }
    std::string getUnsafeBufferUsageAttributeTextAt(
        SourceLocation, StringRef WSSuffix = "") const override {
      return "";
    }
  };

  FixableGadgetList FixableGadgets;
  WarningGadgetList WarningGadgets;
  DeclUseTracker Tracker;
  MockReporter IgnoreHandler;

  findGadgets(FD->getBody(), FD->getASTContext(), IgnoreHandler, false,
              FixableGadgets, WarningGadgets, Tracker);

  std::set<const Expr *> Result;
  for (auto &G : WarningGadgets) {
    for (const Expr *E : G->getUnsafePtrs()) {
      Result.insert(E);
    }
  }

  return Result;
}

struct WarningGadgetSets {
  std::map<const VarDecl *, std::set<const WarningGadget *>,
           // To keep keys sorted by their locations in the map so that the
           // order is deterministic:
           CompareNode<VarDecl>>
      byVar;
  // These Gadgets are not related to pointer variables (e. g. temporaries).
  llvm::SmallVector<const WarningGadget *, 16> noVar;
};

static WarningGadgetSets
groupWarningGadgetsByVar(const WarningGadgetList &AllUnsafeOperations) {
  WarningGadgetSets result;
  // If some gadgets cover more than one
  // variable, they'll appear more than once in the map.
  for (auto &G : AllUnsafeOperations) {
    DeclUseList ClaimedVarUseSites = G->getClaimedVarUseSites();

    bool AssociatedWithVarDecl = false;
    for (const DeclRefExpr *DRE : ClaimedVarUseSites) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        result.byVar[VD].insert(G.get());
        AssociatedWithVarDecl = true;
      }
    }

    if (!AssociatedWithVarDecl) {
      result.noVar.push_back(G.get());
      continue;
    }
  }
  return result;
}

struct FixableGadgetSets {
  std::map<const VarDecl *, std::set<const FixableGadget *>,
           // To keep keys sorted by their locations in the map so that the
           // order is deterministic:
           CompareNode<VarDecl>>
      byVar;
};

static FixableGadgetSets
groupFixablesByVar(FixableGadgetList &&AllFixableOperations) {
  FixableGadgetSets FixablesForUnsafeVars;
  for (auto &F : AllFixableOperations) {
    DeclUseList DREs = F->getClaimedVarUseSites();

    for (const DeclRefExpr *DRE : DREs) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        FixablesForUnsafeVars.byVar[VD].insert(F.get());
      }
    }
  }
  return FixablesForUnsafeVars;
}

bool clang::internal::anyConflict(const SmallVectorImpl<FixItHint> &FixIts,
                                  const SourceManager &SM) {
  // A simple interval overlap detection algorithm.  Sorts all ranges by their
  // begin location then finds the first overlap in one pass.
  std::vector<const FixItHint *> All; // a copy of `FixIts`

  for (const FixItHint &H : FixIts)
    All.push_back(&H);
  std::sort(All.begin(), All.end(),
            [&SM](const FixItHint *H1, const FixItHint *H2) {
              return SM.isBeforeInTranslationUnit(H1->RemoveRange.getBegin(),
                                                  H2->RemoveRange.getBegin());
            });

  const FixItHint *CurrHint = nullptr;

  for (const FixItHint *Hint : All) {
    if (!CurrHint ||
        SM.isBeforeInTranslationUnit(CurrHint->RemoveRange.getEnd(),
                                     Hint->RemoveRange.getBegin())) {
      // Either to initialize `CurrHint` or `CurrHint` does not
      // overlap with `Hint`:
      CurrHint = Hint;
    } else
      // In case `Hint` overlaps the `CurrHint`, we found at least one
      // conflict:
      return true;
  }
  return false;
}

std::optional<FixItList>
PtrToPtrAssignmentGadget::getFixits(const FixitStrategy &S) const {
  const auto *LeftVD = cast<VarDecl>(PtrLHS->getDecl());
  const auto *RightVD = cast<VarDecl>(PtrRHS->getDecl());
  switch (S.lookup(LeftVD)) {
  case FixitStrategy::Kind::Span:
    if (S.lookup(RightVD) == FixitStrategy::Kind::Span)
      return FixItList{};
    return std::nullopt;
  case FixitStrategy::Kind::Wontfix:
    return std::nullopt;
  case FixitStrategy::Kind::Iterator:
  case FixitStrategy::Kind::Array:
    return std::nullopt;
  case FixitStrategy::Kind::Vector:
    llvm_unreachable("unsupported strategies for FixableGadgets");
  }
  return std::nullopt;
}

/// \returns fixit that adds .data() call after \DRE.
static inline std::optional<FixItList> createDataFixit(const ASTContext &Ctx,
                                                       const DeclRefExpr *DRE);

std::optional<FixItList>
CArrayToPtrAssignmentGadget::getFixits(const FixitStrategy &S) const {
  const auto *LeftVD = cast<VarDecl>(PtrLHS->getDecl());
  const auto *RightVD = cast<VarDecl>(PtrRHS->getDecl());
  // TLDR: Implementing fixits for non-Wontfix strategy on both LHS and RHS is
  // non-trivial.
  //
  // CArrayToPtrAssignmentGadget doesn't have strategy implications because
  // constant size array propagates its bounds. Because of that LHS and RHS are
  // addressed by two different fixits.
  //
  // At the same time FixitStrategy S doesn't reflect what group a fixit belongs
  // to and can't be generally relied on in multi-variable Fixables!
  //
  // E. g. If an instance of this gadget is fixing variable on LHS then the
  // variable on RHS is fixed by a different fixit and its strategy for LHS
  // fixit is as if Wontfix.
  //
  // The only exception is Wontfix strategy for a given variable as that is
  // valid for any fixit produced for the given input source code.
  if (S.lookup(LeftVD) == FixitStrategy::Kind::Span) {
    if (S.lookup(RightVD) == FixitStrategy::Kind::Wontfix) {
      return FixItList{};
    }
  } else if (S.lookup(LeftVD) == FixitStrategy::Kind::Wontfix) {
    if (S.lookup(RightVD) == FixitStrategy::Kind::Array) {
      return createDataFixit(RightVD->getASTContext(), PtrRHS);
    }
  }
  return std::nullopt;
}

std::optional<FixItList>
PointerInitGadget::getFixits(const FixitStrategy &S) const {
  const auto *LeftVD = PtrInitLHS;
  const auto *RightVD = cast<VarDecl>(PtrInitRHS->getDecl());
  switch (S.lookup(LeftVD)) {
  case FixitStrategy::Kind::Span:
    if (S.lookup(RightVD) == FixitStrategy::Kind::Span)
      return FixItList{};
    return std::nullopt;
  case FixitStrategy::Kind::Wontfix:
    return std::nullopt;
  case FixitStrategy::Kind::Iterator:
  case FixitStrategy::Kind::Array:
    return std::nullopt;
  case FixitStrategy::Kind::Vector:
    llvm_unreachable("unsupported strategies for FixableGadgets");
  }
  return std::nullopt;
}

static bool isNonNegativeIntegerExpr(const Expr *Expr, const VarDecl *VD,
                                     const ASTContext &Ctx) {
  if (auto ConstVal = Expr->getIntegerConstantExpr(Ctx)) {
    if (ConstVal->isNegative())
      return false;
  } else if (!Expr->getType()->isUnsignedIntegerType())
    return false;
  return true;
}

std::optional<FixItList>
ULCArraySubscriptGadget::getFixits(const FixitStrategy &S) const {
  if (const auto *DRE =
          dyn_cast<DeclRefExpr>(Node->getBase()->IgnoreImpCasts()))
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      switch (S.lookup(VD)) {
      case FixitStrategy::Kind::Span: {

        // If the index has a negative constant value, we give up as no valid
        // fix-it can be generated:
        const ASTContext &Ctx = // FIXME: we need ASTContext to be passed in!
            VD->getASTContext();
        if (!isNonNegativeIntegerExpr(Node->getIdx(), VD, Ctx))
          return std::nullopt;
        // no-op is a good fix-it, otherwise
        return FixItList{};
      }
      case FixitStrategy::Kind::Array:
        return FixItList{};
      case FixitStrategy::Kind::Wontfix:
      case FixitStrategy::Kind::Iterator:
      case FixitStrategy::Kind::Vector:
        llvm_unreachable("unsupported strategies for FixableGadgets");
      }
    }
  return std::nullopt;
}

static std::optional<FixItList> // forward declaration
fixUPCAddressofArraySubscriptWithSpan(const UnaryOperator *Node);

std::optional<FixItList>
UPCAddressofArraySubscriptGadget::getFixits(const FixitStrategy &S) const {
  auto DREs = getClaimedVarUseSites();
  const auto *VD = cast<VarDecl>(DREs.front()->getDecl());

  switch (S.lookup(VD)) {
  case FixitStrategy::Kind::Span:
    return fixUPCAddressofArraySubscriptWithSpan(Node);
  case FixitStrategy::Kind::Wontfix:
  case FixitStrategy::Kind::Iterator:
  case FixitStrategy::Kind::Array:
    return std::nullopt;
  case FixitStrategy::Kind::Vector:
    llvm_unreachable("unsupported strategies for FixableGadgets");
  }
  return std::nullopt; // something went wrong, no fix-it
}

// FIXME: this function should be customizable through format
static StringRef getEndOfLine() {
  static const char *const EOL = "\n";
  return EOL;
}

// Returns the text indicating that the user needs to provide input there:
static std::string
getUserFillPlaceHolder(StringRef HintTextToUser = "placeholder") {
  std::string s = std::string("<# ");
  s += HintTextToUser;
  s += " #>";
  return s;
}

// Return the source location of the last character of the AST `Node`.
template <typename NodeTy>
static std::optional<SourceLocation>
getEndCharLoc(const NodeTy *Node, const SourceManager &SM,
              const LangOptions &LangOpts) {
  if (unsigned TkLen =
          Lexer::MeasureTokenLength(Node->getEndLoc(), SM, LangOpts)) {
    SourceLocation Loc = Node->getEndLoc().getLocWithOffset(TkLen - 1);

    if (Loc.isValid())
      return Loc;
  }
  return std::nullopt;
}

// We cannot fix a variable declaration if it has some other specifiers than the
// type specifier.  Because the source ranges of those specifiers could overlap
// with the source range that is being replaced using fix-its.  Especially when
// we often cannot obtain accurate source ranges of cv-qualified type
// specifiers.
// FIXME: also deal with type attributes
static bool hasUnsupportedSpecifiers(const VarDecl *VD,
                                     const SourceManager &SM) {
  // AttrRangeOverlapping: true if at least one attribute of `VD` overlaps the
  // source range of `VD`:
  bool AttrRangeOverlapping = llvm::any_of(VD->attrs(), [&](Attr *At) -> bool {
    return !(SM.isBeforeInTranslationUnit(At->getRange().getEnd(),
                                          VD->getBeginLoc())) &&
           !(SM.isBeforeInTranslationUnit(VD->getEndLoc(),
                                          At->getRange().getBegin()));
  });
  return VD->isInlineSpecified() || VD->isConstexpr() ||
         VD->hasConstantInitialization() || !VD->hasLocalStorage() ||
         AttrRangeOverlapping;
}

// Returns the `SourceRange` of `D`.  The reason why this function exists is
// that `D->getSourceRange()` may return a range where the end location is the
// starting location of the last token.  The end location of the source range
// returned by this function is the last location of the last token.
static SourceRange getSourceRangeToTokenEnd(const Decl *D,
                                            const SourceManager &SM,
                                            const LangOptions &LangOpts) {
  SourceLocation Begin = D->getBeginLoc();
  SourceLocation
      End = // `D->getEndLoc` should always return the starting location of the
      // last token, so we should get the end of the token
      Lexer::getLocForEndOfToken(D->getEndLoc(), 0, SM, LangOpts);

  return SourceRange(Begin, End);
}

// Returns the text of the name (with qualifiers) of a `FunctionDecl`.
static std::optional<StringRef> getFunNameText(const FunctionDecl *FD,
                                               const SourceManager &SM,
                                               const LangOptions &LangOpts) {
  SourceLocation BeginLoc = FD->getQualifier()
                                ? FD->getQualifierLoc().getBeginLoc()
                                : FD->getNameInfo().getBeginLoc();
  // Note that `FD->getNameInfo().getEndLoc()` returns the begin location of the
  // last token:
  SourceLocation EndLoc = Lexer::getLocForEndOfToken(
      FD->getNameInfo().getEndLoc(), 0, SM, LangOpts);
  SourceRange NameRange{BeginLoc, EndLoc};

  return getRangeText(NameRange, SM, LangOpts);
}

// Returns the text representing a `std::span` type where the element type is
// represented by `EltTyText`.
//
// Note the optional parameter `Qualifiers`: one needs to pass qualifiers
// explicitly if the element type needs to be qualified.
static std::string
getSpanTypeText(StringRef EltTyText,
                std::optional<Qualifiers> Quals = std::nullopt) {
  const char *const SpanOpen = "std::span<";

  if (Quals)
    return SpanOpen + EltTyText.str() + ' ' + Quals->getAsString() + '>';
  return SpanOpen + EltTyText.str() + '>';
}

std::optional<FixItList>
DerefSimplePtrArithFixableGadget::getFixits(const FixitStrategy &s) const {
  const VarDecl *VD = dyn_cast<VarDecl>(BaseDeclRefExpr->getDecl());

  if (VD && s.lookup(VD) == FixitStrategy::Kind::Span) {
    ASTContext &Ctx = VD->getASTContext();
    // std::span can't represent elements before its begin()
    if (auto ConstVal = Offset->getIntegerConstantExpr(Ctx))
      if (ConstVal->isNegative())
        return std::nullopt;

    // note that the expr may (oddly) has multiple layers of parens
    // example:
    //   *((..(pointer + 123)..))
    // goal:
    //   pointer[123]
    // Fix-It:
    //   remove '*('
    //   replace ' + ' with '['
    //   replace ')' with ']'

    // example:
    //   *((..(123 + pointer)..))
    // goal:
    //   123[pointer]
    // Fix-It:
    //   remove '*('
    //   replace ' + ' with '['
    //   replace ')' with ']'

    const Expr *LHS = AddOp->getLHS(), *RHS = AddOp->getRHS();
    const SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LangOpts = Ctx.getLangOpts();
    CharSourceRange StarWithTrailWhitespace =
        clang::CharSourceRange::getCharRange(DerefOp->getOperatorLoc(),
                                             LHS->getBeginLoc());

    std::optional<SourceLocation> LHSLocation = getPastLoc(LHS, SM, LangOpts);
    if (!LHSLocation)
      return std::nullopt;

    CharSourceRange PlusWithSurroundingWhitespace =
        clang::CharSourceRange::getCharRange(*LHSLocation, RHS->getBeginLoc());

    std::optional<SourceLocation> AddOpLocation =
        getPastLoc(AddOp, SM, LangOpts);
    std::optional<SourceLocation> DerefOpLocation =
        getPastLoc(DerefOp, SM, LangOpts);

    if (!AddOpLocation || !DerefOpLocation)
      return std::nullopt;

    CharSourceRange ClosingParenWithPrecWhitespace =
        clang::CharSourceRange::getCharRange(*AddOpLocation, *DerefOpLocation);

    return FixItList{
        {FixItHint::CreateRemoval(StarWithTrailWhitespace),
         FixItHint::CreateReplacement(PlusWithSurroundingWhitespace, "["),
         FixItHint::CreateReplacement(ClosingParenWithPrecWhitespace, "]")}};
  }
  return std::nullopt; // something wrong or unsupported, give up
}

std::optional<FixItList>
PointerDereferenceGadget::getFixits(const FixitStrategy &S) const {
  const VarDecl *VD = cast<VarDecl>(BaseDeclRefExpr->getDecl());
  switch (S.lookup(VD)) {
  case FixitStrategy::Kind::Span: {
    ASTContext &Ctx = VD->getASTContext();
    SourceManager &SM = Ctx.getSourceManager();
    // Required changes: *(ptr); => (ptr[0]); and *ptr; => ptr[0]
    // Deletes the *operand
    CharSourceRange derefRange = clang::CharSourceRange::getCharRange(
        Op->getBeginLoc(), Op->getBeginLoc().getLocWithOffset(1));
    // Inserts the [0]
    if (auto LocPastOperand =
            getPastLoc(BaseDeclRefExpr, SM, Ctx.getLangOpts())) {
      return FixItList{{FixItHint::CreateRemoval(derefRange),
                        FixItHint::CreateInsertion(*LocPastOperand, "[0]")}};
    }
    break;
  }
  case FixitStrategy::Kind::Iterator:
  case FixitStrategy::Kind::Array:
    return std::nullopt;
  case FixitStrategy::Kind::Vector:
    llvm_unreachable("FixitStrategy not implemented yet!");
  case FixitStrategy::Kind::Wontfix:
    llvm_unreachable("Invalid strategy!");
  }

  return std::nullopt;
}

static inline std::optional<FixItList> createDataFixit(const ASTContext &Ctx,
                                                       const DeclRefExpr *DRE) {
  const SourceManager &SM = Ctx.getSourceManager();
  // Inserts the .data() after the DRE
  std::optional<SourceLocation> EndOfOperand =
      getPastLoc(DRE, SM, Ctx.getLangOpts());

  if (EndOfOperand)
    return FixItList{{FixItHint::CreateInsertion(*EndOfOperand, ".data()")}};

  return std::nullopt;
}

// Generates fix-its replacing an expression of the form UPC(DRE) with
// `DRE.data()`
std::optional<FixItList>
UPCStandalonePointerGadget::getFixits(const FixitStrategy &S) const {
  const auto VD = cast<VarDecl>(Node->getDecl());
  switch (S.lookup(VD)) {
  case FixitStrategy::Kind::Array:
  case FixitStrategy::Kind::Span: {
    return createDataFixit(VD->getASTContext(), Node);
    // FIXME: Points inside a macro expansion.
    break;
  }
  case FixitStrategy::Kind::Wontfix:
  case FixitStrategy::Kind::Iterator:
    return std::nullopt;
  case FixitStrategy::Kind::Vector:
    llvm_unreachable("unsupported strategies for FixableGadgets");
  }

  return std::nullopt;
}

// Generates fix-its replacing an expression of the form `&DRE[e]` with
// `&DRE.data()[e]`:
static std::optional<FixItList>
fixUPCAddressofArraySubscriptWithSpan(const UnaryOperator *Node) {
  const auto *ArraySub = cast<ArraySubscriptExpr>(Node->getSubExpr());
  const auto *DRE = cast<DeclRefExpr>(ArraySub->getBase()->IgnoreImpCasts());
  // FIXME: this `getASTContext` call is costly, we should pass the
  // ASTContext in:
  const ASTContext &Ctx = DRE->getDecl()->getASTContext();
  const Expr *Idx = ArraySub->getIdx();
  const SourceManager &SM = Ctx.getSourceManager();
  const LangOptions &LangOpts = Ctx.getLangOpts();
  std::stringstream SS;
  bool IdxIsLitZero = false;

  if (auto ICE = Idx->getIntegerConstantExpr(Ctx))
    if ((*ICE).isZero())
      IdxIsLitZero = true;
  std::optional<StringRef> DreString = getExprText(DRE, SM, LangOpts);
  if (!DreString)
    return std::nullopt;

  if (IdxIsLitZero) {
    // If the index is literal zero, we produce the most concise fix-it:
    SS << (*DreString).str() << ".data()";
  } else {
    std::optional<StringRef> IndexString = getExprText(Idx, SM, LangOpts);
    if (!IndexString)
      return std::nullopt;

    SS << "&" << (*DreString).str() << ".data()"
       << "[" << (*IndexString).str() << "]";
  }
  return FixItList{
      FixItHint::CreateReplacement(Node->getSourceRange(), SS.str())};
}

std::optional<FixItList>
UUCAddAssignGadget::getFixits(const FixitStrategy &S) const {
  DeclUseList DREs = getClaimedVarUseSites();

  if (DREs.size() != 1)
    return std::nullopt; // In cases of `Ptr += n` where `Ptr` is not a DRE, we
                         // give up
  if (const VarDecl *VD = dyn_cast<VarDecl>(DREs.front()->getDecl())) {
    if (S.lookup(VD) == FixitStrategy::Kind::Span) {
      FixItList Fixes;

      const Stmt *AddAssignNode = Node;
      StringRef varName = VD->getName();
      const ASTContext &Ctx = VD->getASTContext();

      if (!isNonNegativeIntegerExpr(Offset, VD, Ctx))
        return std::nullopt;

      // To transform UUC(p += n) to UUC(p = p.subspan(..)):
      bool NotParenExpr =
          (Offset->IgnoreParens()->getBeginLoc() == Offset->getBeginLoc());
      std::string SS = varName.str() + " = " + varName.str() + ".subspan";
      if (NotParenExpr)
        SS += "(";

      std::optional<SourceLocation> AddAssignLocation = getEndCharLoc(
          AddAssignNode, Ctx.getSourceManager(), Ctx.getLangOpts());
      if (!AddAssignLocation)
        return std::nullopt;

      Fixes.push_back(FixItHint::CreateReplacement(
          SourceRange(AddAssignNode->getBeginLoc(), Node->getOperatorLoc()),
          SS));
      if (NotParenExpr)
        Fixes.push_back(FixItHint::CreateInsertion(
            Offset->getEndLoc().getLocWithOffset(1), ")"));
      return Fixes;
    }
  }
  return std::nullopt; // Not in the cases that we can handle for now, give up.
}

std::optional<FixItList>
UPCPreIncrementGadget::getFixits(const FixitStrategy &S) const {
  DeclUseList DREs = getClaimedVarUseSites();

  if (DREs.size() != 1)
    return std::nullopt; // In cases of `++Ptr` where `Ptr` is not a DRE, we
                         // give up
  if (const VarDecl *VD = dyn_cast<VarDecl>(DREs.front()->getDecl())) {
    if (S.lookup(VD) == FixitStrategy::Kind::Span) {
      FixItList Fixes;
      std::stringstream SS;
      StringRef varName = VD->getName();
      const ASTContext &Ctx = VD->getASTContext();

      // To transform UPC(++p) to UPC((p = p.subspan(1)).data()):
      SS << "(" << varName.data() << " = " << varName.data()
         << ".subspan(1)).data()";
      std::optional<SourceLocation> PreIncLocation =
          getEndCharLoc(Node, Ctx.getSourceManager(), Ctx.getLangOpts());
      if (!PreIncLocation)
        return std::nullopt;

      Fixes.push_back(FixItHint::CreateReplacement(
          SourceRange(Node->getBeginLoc(), *PreIncLocation), SS.str()));
      return Fixes;
    }
  }
  return std::nullopt; // Not in the cases that we can handle for now, give up.
}

// For a non-null initializer `Init` of `T *` type, this function returns
// `FixItHint`s producing a list initializer `{Init,  S}` as a part of a fix-it
// to output stream.
// In many cases, this function cannot figure out the actual extent `S`.  It
// then will use a place holder to replace `S` to ask users to fill `S` in.  The
// initializer shall be used to initialize a variable of type `std::span<T>`.
// In some cases (e. g. constant size array) the initializer should remain
// unchanged and the function returns empty list. In case the function can't
// provide the right fixit it will return nullopt.
//
// FIXME: Support multi-level pointers
//
// Parameters:
//   `Init` a pointer to the initializer expression
//   `Ctx` a reference to the ASTContext
static std::optional<FixItList>
FixVarInitializerWithSpan(const Expr *Init, ASTContext &Ctx,
                          const StringRef UserFillPlaceHolder) {
  const SourceManager &SM = Ctx.getSourceManager();
  const LangOptions &LangOpts = Ctx.getLangOpts();

  // If `Init` has a constant value that is (or equivalent to) a
  // NULL pointer, we use the default constructor to initialize the span
  // object, i.e., a `std:span` variable declaration with no initializer.
  // So the fix-it is just to remove the initializer.
  if (Init->isNullPointerConstant(
          Ctx,
          // FIXME: Why does this function not ask for `const ASTContext
          // &`? It should. Maybe worth an NFC patch later.
          Expr::NullPointerConstantValueDependence::
              NPC_ValueDependentIsNotNull)) {
    std::optional<SourceLocation> InitLocation =
        getEndCharLoc(Init, SM, LangOpts);
    if (!InitLocation)
      return std::nullopt;

    SourceRange SR(Init->getBeginLoc(), *InitLocation);

    return FixItList{FixItHint::CreateRemoval(SR)};
  }

  FixItList FixIts{};
  std::string ExtentText = UserFillPlaceHolder.data();
  StringRef One = "1";

  // Insert `{` before `Init`:
  FixIts.push_back(FixItHint::CreateInsertion(Init->getBeginLoc(), "{"));
  // Try to get the data extent. Break into different cases:
  if (auto CxxNew = dyn_cast<CXXNewExpr>(Init->IgnoreImpCasts())) {
    // In cases `Init` is `new T[n]` and there is no explicit cast over
    // `Init`, we know that `Init` must evaluates to a pointer to `n` objects
    // of `T`. So the extent is `n` unless `n` has side effects.  Similar but
    // simpler for the case where `Init` is `new T`.
    if (const Expr *Ext = CxxNew->getArraySize().value_or(nullptr)) {
      if (!Ext->HasSideEffects(Ctx)) {
        std::optional<StringRef> ExtentString = getExprText(Ext, SM, LangOpts);
        if (!ExtentString)
          return std::nullopt;
        ExtentText = *ExtentString;
      }
    } else if (!CxxNew->isArray())
      // Although the initializer is not allocating a buffer, the pointer
      // variable could still be used in buffer access operations.
      ExtentText = One;
  } else if (Ctx.getAsConstantArrayType(Init->IgnoreImpCasts()->getType())) {
    // std::span has a single parameter constructor for initialization with
    // constant size array. The size is auto-deduced as the constructor is a
    // function template. The correct fixit is empty - no changes should happen.
    return FixItList{};
  } else {
    // In cases `Init` is of the form `&Var` after stripping of implicit
    // casts, where `&` is the built-in operator, the extent is 1.
    if (auto AddrOfExpr = dyn_cast<UnaryOperator>(Init->IgnoreImpCasts()))
      if (AddrOfExpr->getOpcode() == UnaryOperatorKind::UO_AddrOf &&
          isa_and_present<DeclRefExpr>(AddrOfExpr->getSubExpr()))
        ExtentText = One;
    // TODO: we can handle more cases, e.g., `&a[0]`, `&a`, `std::addressof`,
    // and explicit casting, etc. etc.
  }

  SmallString<32> StrBuffer{};
  std::optional<SourceLocation> LocPassInit = getPastLoc(Init, SM, LangOpts);

  if (!LocPassInit)
    return std::nullopt;

  StrBuffer.append(", ");
  StrBuffer.append(ExtentText);
  StrBuffer.append("}");
  FixIts.push_back(FixItHint::CreateInsertion(*LocPassInit, StrBuffer.str()));
  return FixIts;
}

#ifndef NDEBUG
#define DEBUG_NOTE_DECL_FAIL(D, Msg)                                           \
  Handler.addDebugNoteForVar((D), (D)->getBeginLoc(),                          \
                             "failed to produce fixit for declaration '" +     \
                                 (D)->getNameAsString() + "'" + (Msg))
#else
#define DEBUG_NOTE_DECL_FAIL(D, Msg)
#endif

// For the given variable declaration with a pointer-to-T type, returns the text
// `std::span<T>`.  If it is unable to generate the text, returns
// `std::nullopt`.
static std::optional<std::string>
createSpanTypeForVarDecl(const VarDecl *VD, const ASTContext &Ctx) {
  assert(VD->getType()->isPointerType());

  std::optional<Qualifiers> PteTyQualifiers = std::nullopt;
  std::optional<std::string> PteTyText = getPointeeTypeText(
      VD, Ctx.getSourceManager(), Ctx.getLangOpts(), &PteTyQualifiers);

  if (!PteTyText)
    return std::nullopt;

  std::string SpanTyText = "std::span<";

  SpanTyText.append(*PteTyText);
  // Append qualifiers to span element type if any:
  if (PteTyQualifiers) {
    SpanTyText.append(" ");
    SpanTyText.append(PteTyQualifiers->getAsString());
  }
  SpanTyText.append(">");
  return SpanTyText;
}

// For a `VarDecl` of the form `T  * var (= Init)?`, this
// function generates fix-its that
//  1) replace `T * var` with `std::span<T> var`; and
//  2) change `Init` accordingly to a span constructor, if it exists.
//
// FIXME: support Multi-level pointers
//
// Parameters:
//   `D` a pointer the variable declaration node
//   `Ctx` a reference to the ASTContext
//   `UserFillPlaceHolder` the user-input placeholder text
// Returns:
//    the non-empty fix-it list, if fix-its are successfuly generated; empty
//    list otherwise.
static FixItList fixLocalVarDeclWithSpan(const VarDecl *D, ASTContext &Ctx,
                                         const StringRef UserFillPlaceHolder,
                                         UnsafeBufferUsageHandler &Handler) {
  if (hasUnsupportedSpecifiers(D, Ctx.getSourceManager()))
    return {};

  FixItList FixIts{};
  std::optional<std::string> SpanTyText = createSpanTypeForVarDecl(D, Ctx);

  if (!SpanTyText) {
    DEBUG_NOTE_DECL_FAIL(D, " : failed to generate 'std::span' type");
    return {};
  }

  // Will hold the text for `std::span<T> Ident`:
  std::stringstream SS;

  SS << *SpanTyText;
  // Fix the initializer if it exists:
  if (const Expr *Init = D->getInit()) {
    std::optional<FixItList> InitFixIts =
        FixVarInitializerWithSpan(Init, Ctx, UserFillPlaceHolder);
    if (!InitFixIts)
      return {};
    FixIts.insert(FixIts.end(), std::make_move_iterator(InitFixIts->begin()),
                  std::make_move_iterator(InitFixIts->end()));
  }
  // For declaration of the form `T * ident = init;`, we want to replace
  // `T * ` with `std::span<T>`.
  // We ignore CV-qualifiers so for `T * const ident;` we also want to replace
  // just `T *` with `std::span<T>`.
  const SourceLocation EndLocForReplacement = D->getTypeSpecEndLoc();
  if (!EndLocForReplacement.isValid()) {
    DEBUG_NOTE_DECL_FAIL(D, " : failed to locate the end of the declaration");
    return {};
  }
  // The only exception is that for `T *ident` we'll add a single space between
  // "std::span<T>" and "ident".
  // FIXME: The condition is false for identifiers expended from macros.
  if (EndLocForReplacement.getLocWithOffset(1) == getVarDeclIdentifierLoc(D))
    SS << " ";

  FixIts.push_back(FixItHint::CreateReplacement(
      SourceRange(D->getBeginLoc(), EndLocForReplacement), SS.str()));
  return FixIts;
}

static bool hasConflictingOverload(const FunctionDecl *FD) {
  return !FD->getDeclContext()->lookup(FD->getDeclName()).isSingleResult();
}

// For a `FunctionDecl`, whose `ParmVarDecl`s are being changed to have new
// types, this function produces fix-its to make the change self-contained.  Let
// 'F' be the entity defined by the original `FunctionDecl` and "NewF" be the
// entity defined by the `FunctionDecl` after the change to the parameters.
// Fix-its produced by this function are
//   1. Add the `[[clang::unsafe_buffer_usage]]` attribute to each declaration
//   of 'F';
//   2. Create a declaration of "NewF" next to each declaration of `F`;
//   3. Create a definition of "F" (as its' original definition is now belongs
//      to "NewF") next to its original definition.  The body of the creating
//      definition calls to "NewF".
//
// Example:
//
// void f(int *p);  // original declaration
// void f(int *p) { // original definition
//    p[5];
// }
//
// To change the parameter `p` to be of `std::span<int>` type, we
// also add overloads:
//
// [[clang::unsafe_buffer_usage]] void f(int *p); // original decl
// void f(std::span<int> p);                      // added overload decl
// void f(std::span<int> p) {     // original def where param is changed
//    p[5];
// }
// [[clang::unsafe_buffer_usage]] void f(int *p) {  // added def
//   return f(std::span(p, <# size #>));
// }
//
static std::optional<FixItList>
createOverloadsForFixedParams(const FixitStrategy &S, const FunctionDecl *FD,
                              const ASTContext &Ctx,
                              UnsafeBufferUsageHandler &Handler) {
  // FIXME: need to make this conflict checking better:
  if (hasConflictingOverload(FD))
    return std::nullopt;

  const SourceManager &SM = Ctx.getSourceManager();
  const LangOptions &LangOpts = Ctx.getLangOpts();
  const unsigned NumParms = FD->getNumParams();
  std::vector<std::string> NewTysTexts(NumParms);
  std::vector<bool> ParmsMask(NumParms, false);
  bool AtLeastOneParmToFix = false;

  for (unsigned i = 0; i < NumParms; i++) {
    const ParmVarDecl *PVD = FD->getParamDecl(i);

    if (S.lookup(PVD) == FixitStrategy::Kind::Wontfix)
      continue;
    if (S.lookup(PVD) != FixitStrategy::Kind::Span)
      // Not supported, not suppose to happen:
      return std::nullopt;

    std::optional<Qualifiers> PteTyQuals = std::nullopt;
    std::optional<std::string> PteTyText =
        getPointeeTypeText(PVD, SM, LangOpts, &PteTyQuals);

    if (!PteTyText)
      // something wrong in obtaining the text of the pointee type, give up
      return std::nullopt;
    // FIXME: whether we should create std::span type depends on the
    // FixitStrategy.
    NewTysTexts[i] = getSpanTypeText(*PteTyText, PteTyQuals);
    ParmsMask[i] = true;
    AtLeastOneParmToFix = true;
  }
  if (!AtLeastOneParmToFix)
    // No need to create function overloads:
    return {};
  // FIXME Respect indentation of the original code.

  // A lambda that creates the text representation of a function declaration
  // with the new type signatures:
  const auto NewOverloadSignatureCreator =
      [&SM, &LangOpts, &NewTysTexts,
       &ParmsMask](const FunctionDecl *FD) -> std::optional<std::string> {
    std::stringstream SS;

    SS << ";";
    SS << getEndOfLine().str();
    // Append: ret-type func-name "("
    if (auto Prefix = getRangeText(
            SourceRange(FD->getBeginLoc(), (*FD->param_begin())->getBeginLoc()),
            SM, LangOpts))
      SS << Prefix->str();
    else
      return std::nullopt; // give up
    // Append: parameter-type-list
    const unsigned NumParms = FD->getNumParams();

    for (unsigned i = 0; i < NumParms; i++) {
      const ParmVarDecl *Parm = FD->getParamDecl(i);

      if (Parm->isImplicit())
        continue;
      if (ParmsMask[i]) {
        // This `i`-th parameter will be fixed with `NewTysTexts[i]` being its
        // new type:
        SS << NewTysTexts[i];
        // print parameter name if provided:
        if (IdentifierInfo *II = Parm->getIdentifier())
          SS << ' ' << II->getName().str();
      } else if (auto ParmTypeText =
                     getRangeText(getSourceRangeToTokenEnd(Parm, SM, LangOpts),
                                  SM, LangOpts)) {
        // print the whole `Parm` without modification:
        SS << ParmTypeText->str();
      } else
        return std::nullopt; // something wrong, give up
      if (i != NumParms - 1)
        SS << ", ";
    }
    SS << ")";
    return SS.str();
  };

  // A lambda that creates the text representation of a function definition with
  // the original signature:
  const auto OldOverloadDefCreator =
      [&Handler, &SM, &LangOpts, &NewTysTexts,
       &ParmsMask](const FunctionDecl *FD) -> std::optional<std::string> {
    std::stringstream SS;

    SS << getEndOfLine().str();
    // Append: attr-name ret-type func-name "(" param-list ")" "{"
    if (auto FDPrefix = getRangeText(
            SourceRange(FD->getBeginLoc(), FD->getBody()->getBeginLoc()), SM,
            LangOpts))
      SS << Handler.getUnsafeBufferUsageAttributeTextAt(FD->getBeginLoc(), " ")
         << FDPrefix->str() << "{";
    else
      return std::nullopt;
    // Append: "return" func-name "("
    if (auto FunQualName = getFunNameText(FD, SM, LangOpts))
      SS << "return " << FunQualName->str() << "(";
    else
      return std::nullopt;

    // Append: arg-list
    const unsigned NumParms = FD->getNumParams();
    for (unsigned i = 0; i < NumParms; i++) {
      const ParmVarDecl *Parm = FD->getParamDecl(i);

      if (Parm->isImplicit())
        continue;
      // FIXME: If a parameter has no name, it is unused in the
      // definition. So we could just leave it as it is.
      if (!Parm->getIdentifier())
        // If a parameter of a function definition has no name:
        return std::nullopt;
      if (ParmsMask[i])
        // This is our spanified paramter!
        SS << NewTysTexts[i] << "(" << Parm->getIdentifier()->getName().str()
           << ", " << getUserFillPlaceHolder("size") << ")";
      else
        SS << Parm->getIdentifier()->getName().str();
      if (i != NumParms - 1)
        SS << ", ";
    }
    // finish call and the body
    SS << ");}" << getEndOfLine().str();
    // FIXME: 80-char line formatting?
    return SS.str();
  };

  FixItList FixIts{};
  for (FunctionDecl *FReDecl : FD->redecls()) {
    std::optional<SourceLocation> Loc = getPastLoc(FReDecl, SM, LangOpts);

    if (!Loc)
      return {};
    if (FReDecl->isThisDeclarationADefinition()) {
      assert(FReDecl == FD && "inconsistent function definition");
      // Inserts a definition with the old signature to the end of
      // `FReDecl`:
      if (auto OldOverloadDef = OldOverloadDefCreator(FReDecl))
        FixIts.emplace_back(FixItHint::CreateInsertion(*Loc, *OldOverloadDef));
      else
        return {}; // give up
    } else {
      // Adds the unsafe-buffer attribute (if not already there) to `FReDecl`:
      if (!FReDecl->hasAttr<UnsafeBufferUsageAttr>()) {
        FixIts.emplace_back(FixItHint::CreateInsertion(
            FReDecl->getBeginLoc(), Handler.getUnsafeBufferUsageAttributeTextAt(
                                        FReDecl->getBeginLoc(), " ")));
      }
      // Inserts a declaration with the new signature to the end of `FReDecl`:
      if (auto NewOverloadDecl = NewOverloadSignatureCreator(FReDecl))
        FixIts.emplace_back(FixItHint::CreateInsertion(*Loc, *NewOverloadDecl));
      else
        return {};
    }
  }
  return FixIts;
}

// To fix a `ParmVarDecl` to be of `std::span` type.
static FixItList fixParamWithSpan(const ParmVarDecl *PVD, const ASTContext &Ctx,
                                  UnsafeBufferUsageHandler &Handler) {
  if (hasUnsupportedSpecifiers(PVD, Ctx.getSourceManager())) {
    DEBUG_NOTE_DECL_FAIL(PVD, " : has unsupport specifier(s)");
    return {};
  }
  if (PVD->hasDefaultArg()) {
    // FIXME: generate fix-its for default values:
    DEBUG_NOTE_DECL_FAIL(PVD, " : has default arg");
    return {};
  }

  std::optional<Qualifiers> PteTyQualifiers = std::nullopt;
  std::optional<std::string> PteTyText = getPointeeTypeText(
      PVD, Ctx.getSourceManager(), Ctx.getLangOpts(), &PteTyQualifiers);

  if (!PteTyText) {
    DEBUG_NOTE_DECL_FAIL(PVD, " : invalid pointee type");
    return {};
  }

  std::optional<StringRef> PVDNameText = PVD->getIdentifier()->getName();

  if (!PVDNameText) {
    DEBUG_NOTE_DECL_FAIL(PVD, " : invalid identifier name");
    return {};
  }

  std::stringstream SS;
  std::optional<std::string> SpanTyText = createSpanTypeForVarDecl(PVD, Ctx);

  if (PteTyQualifiers)
    // Append qualifiers if they exist:
    SS << getSpanTypeText(*PteTyText, PteTyQualifiers);
  else
    SS << getSpanTypeText(*PteTyText);
  // Append qualifiers to the type of the parameter:
  if (PVD->getType().hasQualifiers())
    SS << ' ' << PVD->getType().getQualifiers().getAsString();
  // Append parameter's name:
  SS << ' ' << PVDNameText->str();
  // Add replacement fix-it:
  return {FixItHint::CreateReplacement(PVD->getSourceRange(), SS.str())};
}

static FixItList fixVariableWithSpan(const VarDecl *VD,
                                     const DeclUseTracker &Tracker,
                                     ASTContext &Ctx,
                                     UnsafeBufferUsageHandler &Handler) {
  const DeclStmt *DS = Tracker.lookupDecl(VD);
  if (!DS) {
    DEBUG_NOTE_DECL_FAIL(VD,
                         " : variables declared this way not implemented yet");
    return {};
  }
  if (!DS->isSingleDecl()) {
    // FIXME: to support handling multiple `VarDecl`s in a single `DeclStmt`
    DEBUG_NOTE_DECL_FAIL(VD, " : multiple VarDecls");
    return {};
  }
  // Currently DS is an unused variable but we'll need it when
  // non-single decls are implemented, where the pointee type name
  // and the '*' are spread around the place.
  (void)DS;

  // FIXME: handle cases where DS has multiple declarations
  return fixLocalVarDeclWithSpan(VD, Ctx, getUserFillPlaceHolder(), Handler);
}

static FixItList fixVarDeclWithArray(const VarDecl *D, const ASTContext &Ctx,
                                     UnsafeBufferUsageHandler &Handler) {
  FixItList FixIts{};

  // Note: the code below expects the declaration to not use any type sugar like
  // typedef.
  if (auto CAT = Ctx.getAsConstantArrayType(D->getType())) {
    const QualType &ArrayEltT = CAT->getElementType();
    assert(!ArrayEltT.isNull() && "Trying to fix a non-array type variable!");
    // FIXME: support multi-dimensional arrays
    if (isa<clang::ArrayType>(ArrayEltT.getCanonicalType()))
      return {};

    const SourceLocation IdentifierLoc = getVarDeclIdentifierLoc(D);

    // Get the spelling of the element type as written in the source file
    // (including macros, etc.).
    auto MaybeElemTypeTxt =
        getRangeText({D->getBeginLoc(), IdentifierLoc}, Ctx.getSourceManager(),
                     Ctx.getLangOpts());
    if (!MaybeElemTypeTxt)
      return {};
    const llvm::StringRef ElemTypeTxt = MaybeElemTypeTxt->trim();

    // Find the '[' token.
    std::optional<Token> NextTok = Lexer::findNextToken(
        IdentifierLoc, Ctx.getSourceManager(), Ctx.getLangOpts());
    while (NextTok && !NextTok->is(tok::l_square) &&
           NextTok->getLocation() <= D->getSourceRange().getEnd())
      NextTok = Lexer::findNextToken(NextTok->getLocation(),
                                     Ctx.getSourceManager(), Ctx.getLangOpts());
    if (!NextTok)
      return {};
    const SourceLocation LSqBracketLoc = NextTok->getLocation();

    // Get the spelling of the array size as written in the source file
    // (including macros, etc.).
    auto MaybeArraySizeTxt = getRangeText(
        {LSqBracketLoc.getLocWithOffset(1), D->getTypeSpecEndLoc()},
        Ctx.getSourceManager(), Ctx.getLangOpts());
    if (!MaybeArraySizeTxt)
      return {};
    const llvm::StringRef ArraySizeTxt = MaybeArraySizeTxt->trim();
    if (ArraySizeTxt.empty()) {
      // FIXME: Support array size getting determined from the initializer.
      // Examples:
      //    int arr1[] = {0, 1, 2};
      //    int arr2{3, 4, 5};
      // We might be able to preserve the non-specified size with `auto` and
      // `std::to_array`:
      //    auto arr1 = std::to_array<int>({0, 1, 2});
      return {};
    }

    std::optional<StringRef> IdentText =
        getVarDeclIdentifierText(D, Ctx.getSourceManager(), Ctx.getLangOpts());

    if (!IdentText) {
      DEBUG_NOTE_DECL_FAIL(D, " : failed to locate the identifier");
      return {};
    }

    SmallString<32> Replacement;
    llvm::raw_svector_ostream OS(Replacement);
    OS << "std::array<" << ElemTypeTxt << ", " << ArraySizeTxt << "> "
       << IdentText->str();

    FixIts.push_back(FixItHint::CreateReplacement(
        SourceRange{D->getBeginLoc(), D->getTypeSpecEndLoc()}, OS.str()));
  }

  return FixIts;
}

static FixItList fixVariableWithArray(const VarDecl *VD,
                                      const DeclUseTracker &Tracker,
                                      const ASTContext &Ctx,
                                      UnsafeBufferUsageHandler &Handler) {
  const DeclStmt *DS = Tracker.lookupDecl(VD);
  assert(DS && "Fixing non-local variables not implemented yet!");
  if (!DS->isSingleDecl()) {
    // FIXME: to support handling multiple `VarDecl`s in a single `DeclStmt`
    return {};
  }
  // Currently DS is an unused variable but we'll need it when
  // non-single decls are implemented, where the pointee type name
  // and the '*' are spread around the place.
  (void)DS;

  // FIXME: handle cases where DS has multiple declarations
  return fixVarDeclWithArray(VD, Ctx, Handler);
}

// TODO: we should be consistent to use `std::nullopt` to represent no-fix due
// to any unexpected problem.
static FixItList
fixVariable(const VarDecl *VD, FixitStrategy::Kind K,
            /* The function decl under analysis */ const Decl *D,
            const DeclUseTracker &Tracker, ASTContext &Ctx,
            UnsafeBufferUsageHandler &Handler) {
  if (const auto *PVD = dyn_cast<ParmVarDecl>(VD)) {
    auto *FD = dyn_cast<clang::FunctionDecl>(PVD->getDeclContext());
    if (!FD || FD != D) {
      // `FD != D` means that `PVD` belongs to a function that is not being
      // analyzed currently.  Thus `FD` may not be complete.
      DEBUG_NOTE_DECL_FAIL(VD, " : function not currently analyzed");
      return {};
    }

    // TODO If function has a try block we can't change params unless we check
    // also its catch block for their use.
    // FIXME We might support static class methods, some select methods,
    // operators and possibly lamdas.
    if (FD->isMain() || FD->isConstexpr() ||
        FD->getTemplatedKind() != FunctionDecl::TemplatedKind::TK_NonTemplate ||
        FD->isVariadic() ||
        // also covers call-operator of lamdas
        isa<CXXMethodDecl>(FD) ||
        // skip when the function body is a try-block
        (FD->hasBody() && isa<CXXTryStmt>(FD->getBody())) ||
        FD->isOverloadedOperator()) {
      DEBUG_NOTE_DECL_FAIL(VD, " : unsupported function decl");
      return {}; // TODO test all these cases
    }
  }

  switch (K) {
  case FixitStrategy::Kind::Span: {
    if (VD->getType()->isPointerType()) {
      if (const auto *PVD = dyn_cast<ParmVarDecl>(VD))
        return fixParamWithSpan(PVD, Ctx, Handler);

      if (VD->isLocalVarDecl())
        return fixVariableWithSpan(VD, Tracker, Ctx, Handler);
    }
    DEBUG_NOTE_DECL_FAIL(VD, " : not a pointer");
    return {};
  }
  case FixitStrategy::Kind::Array: {
    if (VD->isLocalVarDecl() && Ctx.getAsConstantArrayType(VD->getType()))
      return fixVariableWithArray(VD, Tracker, Ctx, Handler);

    DEBUG_NOTE_DECL_FAIL(VD, " : not a local const-size array");
    return {};
  }
  case FixitStrategy::Kind::Iterator:
  case FixitStrategy::Kind::Vector:
    llvm_unreachable("FixitStrategy not implemented yet!");
  case FixitStrategy::Kind::Wontfix:
    llvm_unreachable("Invalid strategy!");
  }
  llvm_unreachable("Unknown strategy!");
}

// Returns true iff there exists a `FixItHint` 'h' in `FixIts` such that the
// `RemoveRange` of 'h' overlaps with a macro use.
static bool overlapWithMacro(const FixItList &FixIts) {
  // FIXME: For now we only check if the range (or the first token) is (part of)
  // a macro expansion.  Ideally, we want to check for all tokens in the range.
  return llvm::any_of(FixIts, [](const FixItHint &Hint) {
    auto Range = Hint.RemoveRange;
    if (Range.getBegin().isMacroID() || Range.getEnd().isMacroID())
      // If the range (or the first token) is (part of) a macro expansion:
      return true;
    return false;
  });
}

// Returns true iff `VD` is a parameter of the declaration `D`:
static bool isParameterOf(const VarDecl *VD, const Decl *D) {
  return isa<ParmVarDecl>(VD) &&
         VD->getDeclContext() == dyn_cast<DeclContext>(D);
}

// Erases variables in `FixItsForVariable`, if such a variable has an unfixable
// group mate.  A variable `v` is unfixable iff `FixItsForVariable` does not
// contain `v`.
static void eraseVarsForUnfixableGroupMates(
    std::map<const VarDecl *, FixItList> &FixItsForVariable,
    const VariableGroupsManager &VarGrpMgr) {
  // Variables will be removed from `FixItsForVariable`:
  SmallVector<const VarDecl *, 8> ToErase;

  for (const auto &[VD, Ignore] : FixItsForVariable) {
    VarGrpRef Grp = VarGrpMgr.getGroupOfVar(VD);
    if (llvm::any_of(Grp,
                     [&FixItsForVariable](const VarDecl *GrpMember) -> bool {
                       return !FixItsForVariable.count(GrpMember);
                     })) {
      // At least one group member cannot be fixed, so we have to erase the
      // whole group:
      for (const VarDecl *Member : Grp)
        ToErase.push_back(Member);
    }
  }
  for (auto *VarToErase : ToErase)
    FixItsForVariable.erase(VarToErase);
}

// Returns the fix-its that create bounds-safe function overloads for the
// function `D`, if `D`'s parameters will be changed to safe-types through
// fix-its in `FixItsForVariable`.
//
// NOTE: In case `D`'s parameters will be changed but bounds-safe function
// overloads cannot created, the whole group that contains the parameters will
// be erased from `FixItsForVariable`.
static FixItList createFunctionOverloadsForParms(
    std::map<const VarDecl *, FixItList> &FixItsForVariable /* mutable */,
    const VariableGroupsManager &VarGrpMgr, const FunctionDecl *FD,
    const FixitStrategy &S, ASTContext &Ctx,
    UnsafeBufferUsageHandler &Handler) {
  FixItList FixItsSharedByParms{};

  std::optional<FixItList> OverloadFixes =
      createOverloadsForFixedParams(S, FD, Ctx, Handler);

  if (OverloadFixes) {
    FixItsSharedByParms.append(*OverloadFixes);
  } else {
    // Something wrong in generating `OverloadFixes`, need to remove the
    // whole group, where parameters are in, from `FixItsForVariable` (Note
    // that all parameters should be in the same group):
    for (auto *Member : VarGrpMgr.getGroupOfParms())
      FixItsForVariable.erase(Member);
  }
  return FixItsSharedByParms;
}

// Constructs self-contained fix-its for each variable in `FixablesForAllVars`.
static std::map<const VarDecl *, FixItList>
getFixIts(FixableGadgetSets &FixablesForAllVars, const FixitStrategy &S,
          ASTContext &Ctx,
          /* The function decl under analysis */ const Decl *D,
          const DeclUseTracker &Tracker, UnsafeBufferUsageHandler &Handler,
          const VariableGroupsManager &VarGrpMgr) {
  // `FixItsForVariable` will map each variable to a set of fix-its directly
  // associated to the variable itself.  Fix-its of distinct variables in
  // `FixItsForVariable` are disjoint.
  std::map<const VarDecl *, FixItList> FixItsForVariable;

  // Populate `FixItsForVariable` with fix-its directly associated with each
  // variable.  Fix-its directly associated to a variable 'v' are the ones
  // produced by the `FixableGadget`s whose claimed variable is 'v'.
  for (const auto &[VD, Fixables] : FixablesForAllVars.byVar) {
    FixItsForVariable[VD] =
        fixVariable(VD, S.lookup(VD), D, Tracker, Ctx, Handler);
    // If we fail to produce Fix-It for the declaration we have to skip the
    // variable entirely.
    if (FixItsForVariable[VD].empty()) {
      FixItsForVariable.erase(VD);
      continue;
    }
    for (const auto &F : Fixables) {
      std::optional<FixItList> Fixits = F->getFixits(S);

      if (Fixits) {
        FixItsForVariable[VD].insert(FixItsForVariable[VD].end(),
                                     Fixits->begin(), Fixits->end());
        continue;
      }
#ifndef NDEBUG
      Handler.addDebugNoteForVar(
          VD, F->getSourceLoc(),
          ("gadget '" + F->getDebugName() + "' refused to produce a fix")
              .str());
#endif
      FixItsForVariable.erase(VD);
      break;
    }
  }

  // `FixItsForVariable` now contains only variables that can be
  // fixed. A variable can be fixed if its' declaration and all Fixables
  // associated to it can all be fixed.

  // To further remove from `FixItsForVariable` variables whose group mates
  // cannot be fixed...
  eraseVarsForUnfixableGroupMates(FixItsForVariable, VarGrpMgr);
  // Now `FixItsForVariable` gets further reduced: a variable is in
  // `FixItsForVariable` iff it can be fixed and all its group mates can be
  // fixed.

  // Fix-its of bounds-safe overloads of `D` are shared by parameters of `D`.
  // That is,  when fixing multiple parameters in one step,  these fix-its will
  // be applied only once (instead of being applied per parameter).
  FixItList FixItsSharedByParms{};

  if (auto *FD = dyn_cast<FunctionDecl>(D))
    FixItsSharedByParms = createFunctionOverloadsForParms(
        FixItsForVariable, VarGrpMgr, FD, S, Ctx, Handler);

  // The map that maps each variable `v` to fix-its for the whole group where
  // `v` is in:
  std::map<const VarDecl *, FixItList> FinalFixItsForVariable{
      FixItsForVariable};

  for (auto &[Var, Ignore] : FixItsForVariable) {
    bool AnyParm = false;
    const auto VarGroupForVD = VarGrpMgr.getGroupOfVar(Var, &AnyParm);

    for (const VarDecl *GrpMate : VarGroupForVD) {
      if (Var == GrpMate)
        continue;
      if (FixItsForVariable.count(GrpMate))
        FinalFixItsForVariable[Var].append(FixItsForVariable[GrpMate]);
    }
    if (AnyParm) {
      // This assertion should never fail.  Otherwise we have a bug.
      assert(!FixItsSharedByParms.empty() &&
             "Should not try to fix a parameter that does not belong to a "
             "FunctionDecl");
      FinalFixItsForVariable[Var].append(FixItsSharedByParms);
    }
  }
  // Fix-its that will be applied in one step shall NOT:
  // 1. overlap with macros or/and templates; or
  // 2. conflict with each other.
  // Otherwise, the fix-its will be dropped.
  for (auto Iter = FinalFixItsForVariable.begin();
       Iter != FinalFixItsForVariable.end();)
    if (overlapWithMacro(Iter->second) ||
        clang::internal::anyConflict(Iter->second, Ctx.getSourceManager())) {
      Iter = FinalFixItsForVariable.erase(Iter);
    } else
      Iter++;
  return FinalFixItsForVariable;
}

template <typename VarDeclIterTy>
static FixitStrategy
getNaiveStrategy(llvm::iterator_range<VarDeclIterTy> UnsafeVars) {
  FixitStrategy S;
  for (const VarDecl *VD : UnsafeVars) {
    if (isa<ConstantArrayType>(VD->getType().getCanonicalType()))
      S.set(VD, FixitStrategy::Kind::Array);
    else
      S.set(VD, FixitStrategy::Kind::Span);
  }
  return S;
}

//  Manages variable groups:
class VariableGroupsManagerImpl : public VariableGroupsManager {
  const std::vector<VarGrpTy> Groups;
  const std::map<const VarDecl *, unsigned> &VarGrpMap;
  const llvm::SetVector<const VarDecl *> &GrpsUnionForParms;

public:
  VariableGroupsManagerImpl(
      const std::vector<VarGrpTy> &Groups,
      const std::map<const VarDecl *, unsigned> &VarGrpMap,
      const llvm::SetVector<const VarDecl *> &GrpsUnionForParms)
      : Groups(Groups), VarGrpMap(VarGrpMap),
        GrpsUnionForParms(GrpsUnionForParms) {}

  VarGrpRef getGroupOfVar(const VarDecl *Var, bool *HasParm) const override {
    if (GrpsUnionForParms.contains(Var)) {
      if (HasParm)
        *HasParm = true;
      return GrpsUnionForParms.getArrayRef();
    }
    if (HasParm)
      *HasParm = false;

    auto It = VarGrpMap.find(Var);

    if (It == VarGrpMap.end())
      return {};
    return Groups[It->second];
  }

  VarGrpRef getGroupOfParms() const override {
    return GrpsUnionForParms.getArrayRef();
  }
};

static void applyGadgets(const Decl *D, FixableGadgetList FixableGadgets,
                         WarningGadgetList WarningGadgets,
                         DeclUseTracker Tracker,
                         UnsafeBufferUsageHandler &Handler,
                         bool EmitSuggestions) {
  if (!EmitSuggestions) {
    // Our job is very easy without suggestions. Just warn about
    // every problematic operation and consider it done. No need to deal
    // with fixable gadgets, no need to group operations by variable.
    for (const auto &G : WarningGadgets) {
      G->handleUnsafeOperation(Handler, /*IsRelatedToDecl=*/false,
                               D->getASTContext());
    }

    // This return guarantees that most of the machine doesn't run when
    // suggestions aren't requested.
    assert(FixableGadgets.empty() &&
           "Fixable gadgets found but suggestions not requested!");
    return;
  }

  // If no `WarningGadget`s ever matched, there is no unsafe operations in the
  //  function under the analysis. No need to fix any Fixables.
  if (!WarningGadgets.empty()) {
    // Gadgets "claim" variables they're responsible for. Once this loop
    // finishes, the tracker will only track DREs that weren't claimed by any
    // gadgets, i.e. not understood by the analysis.
    for (const auto &G : FixableGadgets) {
      for (const auto *DRE : G->getClaimedVarUseSites()) {
        Tracker.claimUse(DRE);
      }
    }
  }

  // If no `WarningGadget`s ever matched, there is no unsafe operations in the
  // function under the analysis.  Thus, it early returns here as there is
  // nothing needs to be fixed.
  //
  // Note this claim is based on the assumption that there is no unsafe
  // variable whose declaration is invisible from the analyzing function.
  // Otherwise, we need to consider if the uses of those unsafe varuables needs
  // fix.
  // So far, we are not fixing any global variables or class members. And,
  // lambdas will be analyzed along with the enclosing function. So this early
  // return is correct for now.
  if (WarningGadgets.empty())
    return;

  WarningGadgetSets UnsafeOps =
      groupWarningGadgetsByVar(std::move(WarningGadgets));
  FixableGadgetSets FixablesForAllVars =
      groupFixablesByVar(std::move(FixableGadgets));

  std::map<const VarDecl *, FixItList> FixItsForVariableGroup;

  // Filter out non-local vars and vars with unclaimed DeclRefExpr-s.
  for (auto it = FixablesForAllVars.byVar.cbegin();
       it != FixablesForAllVars.byVar.cend();) {
    // FIXME: need to deal with global variables later
    if ((!it->first->isLocalVarDecl() && !isa<ParmVarDecl>(it->first))) {
#ifndef NDEBUG
      Handler.addDebugNoteForVar(it->first, it->first->getBeginLoc(),
                                 ("failed to produce fixit for '" +
                                  it->first->getNameAsString() +
                                  "' : neither local nor a parameter"));
#endif
      it = FixablesForAllVars.byVar.erase(it);
    } else if (it->first->getType().getCanonicalType()->isReferenceType()) {
#ifndef NDEBUG
      Handler.addDebugNoteForVar(it->first, it->first->getBeginLoc(),
                                 ("failed to produce fixit for '" +
                                  it->first->getNameAsString() +
                                  "' : has a reference type"));
#endif
      it = FixablesForAllVars.byVar.erase(it);
    } else if (Tracker.hasUnclaimedUses(it->first)) {
      it = FixablesForAllVars.byVar.erase(it);
    } else if (it->first->isInitCapture()) {
#ifndef NDEBUG
      Handler.addDebugNoteForVar(it->first, it->first->getBeginLoc(),
                                 ("failed to produce fixit for '" +
                                  it->first->getNameAsString() +
                                  "' : init capture"));
#endif
      it = FixablesForAllVars.byVar.erase(it);
    } else {
      ++it;
    }
  }

#ifndef NDEBUG
  for (const auto &it : UnsafeOps.byVar) {
    const VarDecl *const UnsafeVD = it.first;
    auto UnclaimedDREs = Tracker.getUnclaimedUses(UnsafeVD);
    if (UnclaimedDREs.empty())
      continue;
    const auto UnfixedVDName = UnsafeVD->getNameAsString();
    for (const clang::DeclRefExpr *UnclaimedDRE : UnclaimedDREs) {
      std::string UnclaimedUseTrace =
          getDREAncestorString(UnclaimedDRE, D->getASTContext());

      Handler.addDebugNoteForVar(
          UnsafeVD, UnclaimedDRE->getBeginLoc(),
          ("failed to produce fixit for '" + UnfixedVDName +
           "' : has an unclaimed use\nThe unclaimed DRE trace: " +
           UnclaimedUseTrace));
    }
  }
#endif

  // Fixpoint iteration for pointer assignments
  using DepMapTy =
      llvm::DenseMap<const VarDecl *, llvm::SetVector<const VarDecl *>>;
  DepMapTy DependenciesMap{};
  DepMapTy PtrAssignmentGraph{};

  for (const auto &it : FixablesForAllVars.byVar) {
    for (const FixableGadget *fixable : it.second) {
      std::optional<std::pair<const VarDecl *, const VarDecl *>> ImplPair =
          fixable->getStrategyImplications();
      if (ImplPair) {
        std::pair<const VarDecl *, const VarDecl *> Impl = std::move(*ImplPair);
        PtrAssignmentGraph[Impl.first].insert(Impl.second);
      }
    }
  }

  /*
   The following code does a BFS traversal of the `PtrAssignmentGraph`
   considering all unsafe vars as starting nodes and constructs an undirected
   graph `DependenciesMap`. Constructing the `DependenciesMap` in this manner
   elimiates all variables that are unreachable from any unsafe var. In other
   words, this removes all dependencies that don't include any unsafe variable
   and consequently don't need any fixit generation.
   Note: A careful reader would observe that the code traverses
   `PtrAssignmentGraph` using `CurrentVar` but adds edges between `Var` and
   `Adj` and not between `CurrentVar` and `Adj`. Both approaches would
   achieve the same result but the one used here dramatically cuts the
   amount of hoops the second part of the algorithm needs to jump, given that
   a lot of these connections become "direct". The reader is advised not to
   imagine how the graph is transformed because of using `Var` instead of
   `CurrentVar`. The reader can continue reading as if `CurrentVar` was used,
   and think about why it's equivalent later.
   */
  std::set<const VarDecl *> VisitedVarsDirected{};
  for (const auto &[Var, ignore] : UnsafeOps.byVar) {
    if (VisitedVarsDirected.find(Var) == VisitedVarsDirected.end()) {

      std::queue<const VarDecl *> QueueDirected{};
      QueueDirected.push(Var);
      while (!QueueDirected.empty()) {
        const VarDecl *CurrentVar = QueueDirected.front();
        QueueDirected.pop();
        VisitedVarsDirected.insert(CurrentVar);
        auto AdjacentNodes = PtrAssignmentGraph[CurrentVar];
        for (const VarDecl *Adj : AdjacentNodes) {
          if (VisitedVarsDirected.find(Adj) == VisitedVarsDirected.end()) {
            QueueDirected.push(Adj);
          }
          DependenciesMap[Var].insert(Adj);
          DependenciesMap[Adj].insert(Var);
        }
      }
    }
  }

  // `Groups` stores the set of Connected Components in the graph.
  std::vector<VarGrpTy> Groups;
  // `VarGrpMap` maps variables that need fix to the groups (indexes) that the
  // variables belong to.  Group indexes refer to the elements in `Groups`.
  // `VarGrpMap` is complete in that every variable that needs fix is in it.
  std::map<const VarDecl *, unsigned> VarGrpMap;
  // The union group over the ones in "Groups" that contain parameters of `D`:
  llvm::SetVector<const VarDecl *>
      GrpsUnionForParms; // these variables need to be fixed in one step

  // Group Connected Components for Unsafe Vars
  // (Dependencies based on pointer assignments)
  std::set<const VarDecl *> VisitedVars{};
  for (const auto &[Var, ignore] : UnsafeOps.byVar) {
    if (VisitedVars.find(Var) == VisitedVars.end()) {
      VarGrpTy &VarGroup = Groups.emplace_back();
      std::queue<const VarDecl *> Queue{};

      Queue.push(Var);
      while (!Queue.empty()) {
        const VarDecl *CurrentVar = Queue.front();
        Queue.pop();
        VisitedVars.insert(CurrentVar);
        VarGroup.push_back(CurrentVar);
        auto AdjacentNodes = DependenciesMap[CurrentVar];
        for (const VarDecl *Adj : AdjacentNodes) {
          if (VisitedVars.find(Adj) == VisitedVars.end()) {
            Queue.push(Adj);
          }
        }
      }

      bool HasParm = false;
      unsigned GrpIdx = Groups.size() - 1;

      for (const VarDecl *V : VarGroup) {
        VarGrpMap[V] = GrpIdx;
        if (!HasParm && isParameterOf(V, D))
          HasParm = true;
      }
      if (HasParm)
        GrpsUnionForParms.insert_range(VarGroup);
    }
  }

  // Remove a `FixableGadget` if the associated variable is not in the graph
  // computed above.  We do not want to generate fix-its for such variables,
  // since they are neither warned nor reachable from a warned one.
  //
  // Note a variable is not warned if it is not directly used in any unsafe
  // operation. A variable `v` is NOT reachable from an unsafe variable, if it
  // does not exist another variable `u` such that `u` is warned and fixing `u`
  // (transitively) implicates fixing `v`.
  //
  // For example,
  // ```
  // void f(int * p) {
  //   int * a = p; *p = 0;
  // }
  // ```
  // `*p = 0` is a fixable gadget associated with a variable `p` that is neither
  // warned nor reachable from a warned one.  If we add `a[5] = 0` to the end of
  // the function above, `p` becomes reachable from a warned variable.
  for (auto I = FixablesForAllVars.byVar.begin();
       I != FixablesForAllVars.byVar.end();) {
    // Note `VisitedVars` contain all the variables in the graph:
    if (!VisitedVars.count((*I).first)) {
      // no such var in graph:
      I = FixablesForAllVars.byVar.erase(I);
    } else
      ++I;
  }

  // We assign strategies to variables that are 1) in the graph and 2) can be
  // fixed. Other variables have the default "Won't fix" strategy.
  FixitStrategy NaiveStrategy = getNaiveStrategy(llvm::make_filter_range(
      VisitedVars, [&FixablesForAllVars](const VarDecl *V) {
        // If a warned variable has no "Fixable", it is considered unfixable:
        return FixablesForAllVars.byVar.count(V);
      }));
  VariableGroupsManagerImpl VarGrpMgr(Groups, VarGrpMap, GrpsUnionForParms);

  if (isa<NamedDecl>(D))
    // The only case where `D` is not a `NamedDecl` is when `D` is a
    // `BlockDecl`. Let's not fix variables in blocks for now
    FixItsForVariableGroup =
        getFixIts(FixablesForAllVars, NaiveStrategy, D->getASTContext(), D,
                  Tracker, Handler, VarGrpMgr);

  for (const auto &G : UnsafeOps.noVar) {
    G->handleUnsafeOperation(Handler, /*IsRelatedToDecl=*/false,
                             D->getASTContext());
  }

  for (const auto &[VD, WarningGadgets] : UnsafeOps.byVar) {
    auto FixItsIt = FixItsForVariableGroup.find(VD);
    Handler.handleUnsafeVariableGroup(VD, VarGrpMgr,
                                      FixItsIt != FixItsForVariableGroup.end()
                                          ? std::move(FixItsIt->second)
                                          : FixItList{},
                                      D, NaiveStrategy);
    for (const auto &G : WarningGadgets) {
      G->handleUnsafeOperation(Handler, /*IsRelatedToDecl=*/true,
                               D->getASTContext());
    }
  }
}

void clang::checkUnsafeBufferUsage(const Decl *D,
                                   UnsafeBufferUsageHandler &Handler,
                                   bool EmitSuggestions) {
#ifndef NDEBUG
  Handler.clearDebugNotes();
#endif

  assert(D);

  SmallVector<Stmt *> Stmts;

  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    // We do not want to visit a Lambda expression defined inside a method
    // independently. Instead, it should be visited along with the outer method.
    // FIXME: do we want to do the same thing for `BlockDecl`s?
    if (const auto *MD = dyn_cast<CXXMethodDecl>(D)) {
      if (MD->getParent()->isLambda() && MD->getParent()->isLocalClass())
        return;
    }

    for (FunctionDecl *FReDecl : FD->redecls()) {
      if (FReDecl->isExternC()) {
        // Do not emit fixit suggestions for functions declared in an
        // extern "C" block.
        EmitSuggestions = false;
        break;
      }
    }

    Stmts.push_back(FD->getBody());

    if (const auto *ID = dyn_cast<CXXConstructorDecl>(D)) {
      for (const CXXCtorInitializer *CI : ID->inits()) {
        Stmts.push_back(CI->getInit());
      }
    }
  } else if (isa<BlockDecl>(D) || isa<ObjCMethodDecl>(D)) {
    Stmts.push_back(D->getBody());
  }

  assert(!Stmts.empty());

  FixableGadgetList FixableGadgets;
  WarningGadgetList WarningGadgets;
  DeclUseTracker Tracker;
  for (Stmt *S : Stmts) {
    findGadgets(S, D->getASTContext(), Handler, EmitSuggestions, FixableGadgets,
                WarningGadgets, Tracker);
  }
  applyGadgets(D, std::move(FixableGadgets), std::move(WarningGadgets),
               std::move(Tracker), Handler, EmitSuggestions);
}
