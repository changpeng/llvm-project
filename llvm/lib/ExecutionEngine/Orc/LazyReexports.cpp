//===---------- LazyReexports.cpp - Utilities for lazy reexports ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/LazyReexports.h"

#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/OrcABISupport.h"
#include "llvm/ExecutionEngine/Orc/Shared/SimplePackedSerialization.h"
#include "llvm/TargetParser/Triple.h"

#define DEBUG_TYPE "orc"

namespace llvm {
namespace orc {

LazyCallThroughManager::LazyCallThroughManager(ExecutionSession &ES,
                                               ExecutorAddr ErrorHandlerAddr,
                                               TrampolinePool *TP)
    : ES(ES), ErrorHandlerAddr(ErrorHandlerAddr), TP(TP) {}

Expected<ExecutorAddr> LazyCallThroughManager::getCallThroughTrampoline(
    JITDylib &SourceJD, SymbolStringPtr SymbolName,
    NotifyResolvedFunction NotifyResolved) {
  assert(TP && "TrampolinePool not set");

  std::lock_guard<std::mutex> Lock(LCTMMutex);
  auto Trampoline = TP->getTrampoline();

  if (!Trampoline)
    return Trampoline.takeError();

  Reexports[*Trampoline] = ReexportsEntry{&SourceJD, std::move(SymbolName)};
  Notifiers[*Trampoline] = std::move(NotifyResolved);
  return *Trampoline;
}

ExecutorAddr LazyCallThroughManager::reportCallThroughError(Error Err) {
  ES.reportError(std::move(Err));
  return ErrorHandlerAddr;
}

Expected<LazyCallThroughManager::ReexportsEntry>
LazyCallThroughManager::findReexport(ExecutorAddr TrampolineAddr) {
  std::lock_guard<std::mutex> Lock(LCTMMutex);
  auto I = Reexports.find(TrampolineAddr);
  if (I == Reexports.end())
    return createStringError(inconvertibleErrorCode(),
                             "Missing reexport for trampoline address %p" +
                                 formatv("{0:x}", TrampolineAddr));
  return I->second;
}

Error LazyCallThroughManager::notifyResolved(ExecutorAddr TrampolineAddr,
                                             ExecutorAddr ResolvedAddr) {
  NotifyResolvedFunction NotifyResolved;
  {
    std::lock_guard<std::mutex> Lock(LCTMMutex);
    auto I = Notifiers.find(TrampolineAddr);
    if (I != Notifiers.end()) {
      NotifyResolved = std::move(I->second);
      Notifiers.erase(I);
    }
  }

  return NotifyResolved ? NotifyResolved(ResolvedAddr) : Error::success();
}

void LazyCallThroughManager::resolveTrampolineLandingAddress(
    ExecutorAddr TrampolineAddr,
    NotifyLandingResolvedFunction NotifyLandingResolved) {

  auto Entry = findReexport(TrampolineAddr);
  if (!Entry)
    return NotifyLandingResolved(reportCallThroughError(Entry.takeError()));

  // Declaring SLS and the callback outside of the call to ES.lookup is a
  // workaround to fix build failures on AIX and on z/OS platforms.
  SymbolLookupSet SLS({Entry->SymbolName});
  auto Callback = [this, TrampolineAddr, SymbolName = Entry->SymbolName,
                   NotifyLandingResolved = std::move(NotifyLandingResolved)](
                      Expected<SymbolMap> Result) mutable {
    if (Result) {
      assert(Result->size() == 1 && "Unexpected result size");
      assert(Result->count(SymbolName) && "Unexpected result value");
      ExecutorAddr LandingAddr = (*Result)[SymbolName].getAddress();

      if (auto Err = notifyResolved(TrampolineAddr, LandingAddr))
        NotifyLandingResolved(reportCallThroughError(std::move(Err)));
      else
        NotifyLandingResolved(LandingAddr);
    } else {
      NotifyLandingResolved(reportCallThroughError(Result.takeError()));
    }
  };

  ES.lookup(LookupKind::Static,
            makeJITDylibSearchOrder(Entry->SourceJD,
                                    JITDylibLookupFlags::MatchAllSymbols),
            std::move(SLS), SymbolState::Ready, std::move(Callback),
            NoDependenciesToRegister);
}

Expected<std::unique_ptr<LazyCallThroughManager>>
createLocalLazyCallThroughManager(const Triple &T, ExecutionSession &ES,
                                  ExecutorAddr ErrorHandlerAddr) {
  switch (T.getArch()) {
  default:
    return make_error<StringError>(
        std::string("No callback manager available for ") + T.str(),
        inconvertibleErrorCode());

  case Triple::aarch64:
  case Triple::aarch64_32:
    return LocalLazyCallThroughManager::Create<OrcAArch64>(ES,
                                                           ErrorHandlerAddr);

  case Triple::x86:
    return LocalLazyCallThroughManager::Create<OrcI386>(ES, ErrorHandlerAddr);

  case Triple::loongarch64:
    return LocalLazyCallThroughManager::Create<OrcLoongArch64>(
        ES, ErrorHandlerAddr);

  case Triple::mips:
    return LocalLazyCallThroughManager::Create<OrcMips32Be>(ES,
                                                            ErrorHandlerAddr);

  case Triple::mipsel:
    return LocalLazyCallThroughManager::Create<OrcMips32Le>(ES,
                                                            ErrorHandlerAddr);

  case Triple::mips64:
  case Triple::mips64el:
    return LocalLazyCallThroughManager::Create<OrcMips64>(ES, ErrorHandlerAddr);

  case Triple::riscv64:
    return LocalLazyCallThroughManager::Create<OrcRiscv64>(ES,
                                                           ErrorHandlerAddr);

  case Triple::x86_64:
    if (T.getOS() == Triple::OSType::Win32)
      return LocalLazyCallThroughManager::Create<OrcX86_64_Win32>(
          ES, ErrorHandlerAddr);
    else
      return LocalLazyCallThroughManager::Create<OrcX86_64_SysV>(
          ES, ErrorHandlerAddr);
  }
}

LazyReexportsMaterializationUnit::LazyReexportsMaterializationUnit(
    LazyCallThroughManager &LCTManager, RedirectableSymbolManager &RSManager,
    JITDylib &SourceJD, SymbolAliasMap CallableAliases, ImplSymbolMap *SrcJDLoc)
    : MaterializationUnit(extractFlags(CallableAliases)),
      LCTManager(LCTManager), RSManager(RSManager), SourceJD(SourceJD),
      CallableAliases(std::move(CallableAliases)), AliaseeTable(SrcJDLoc) {}

StringRef LazyReexportsMaterializationUnit::getName() const {
  return "<Lazy Reexports>";
}

void LazyReexportsMaterializationUnit::materialize(
    std::unique_ptr<MaterializationResponsibility> R) {
  auto RequestedSymbols = R->getRequestedSymbols();

  SymbolAliasMap RequestedAliases;
  for (auto &RequestedSymbol : RequestedSymbols) {
    auto I = CallableAliases.find(RequestedSymbol);
    assert(I != CallableAliases.end() && "Symbol not found in alias map?");
    RequestedAliases[I->first] = std::move(I->second);
    CallableAliases.erase(I);
  }

  if (!CallableAliases.empty())
    if (auto Err = R->replace(lazyReexports(LCTManager, RSManager, SourceJD,
                                            std::move(CallableAliases),
                                            AliaseeTable))) {
      R->getExecutionSession().reportError(std::move(Err));
      R->failMaterialization();
      return;
    }

  SymbolMap Inits;
  for (auto &Alias : RequestedAliases) {
    auto CallThroughTrampoline = LCTManager.getCallThroughTrampoline(
        SourceJD, Alias.second.Aliasee,
        [&TargetJD = R->getTargetJITDylib(), &RSManager = this->RSManager,
         StubSym = Alias.first](ExecutorAddr ResolvedAddr) -> Error {
          return RSManager.redirect(TargetJD, StubSym,
                                    ExecutorSymbolDef(ResolvedAddr, {}));
        });

    if (!CallThroughTrampoline) {
      R->getExecutionSession().reportError(CallThroughTrampoline.takeError());
      R->failMaterialization();
      return;
    }

    Inits[Alias.first] = {*CallThroughTrampoline, Alias.second.AliasFlags};
  }

  if (AliaseeTable != nullptr && !RequestedAliases.empty())
    AliaseeTable->trackImpls(RequestedAliases, &SourceJD);

  if (auto Err = R->replace(std::make_unique<RedirectableMaterializationUnit>(
          RSManager, std::move(Inits)))) {
    R->getExecutionSession().reportError(std::move(Err));
    return R->failMaterialization();
  }
}

void LazyReexportsMaterializationUnit::discard(const JITDylib &JD,
                                               const SymbolStringPtr &Name) {
  assert(CallableAliases.count(Name) &&
         "Symbol not covered by this MaterializationUnit");
  CallableAliases.erase(Name);
}

MaterializationUnit::Interface
LazyReexportsMaterializationUnit::extractFlags(const SymbolAliasMap &Aliases) {
  SymbolFlagsMap SymbolFlags;
  for (auto &KV : Aliases) {
    assert(KV.second.AliasFlags.isCallable() &&
           "Lazy re-exports must be callable symbols");
    SymbolFlags[KV.first] = KV.second.AliasFlags;
  }
  return MaterializationUnit::Interface(std::move(SymbolFlags), nullptr);
}

class LazyReexportsManager::MU : public MaterializationUnit {
public:
  MU(LazyReexportsManager &LRMgr, SymbolAliasMap Reexports)
      : MaterializationUnit(getInterface(Reexports)), LRMgr(LRMgr),
        Reexports(std::move(Reexports)) {}

private:
  Interface getInterface(const SymbolAliasMap &Reexports) {
    SymbolFlagsMap SF;
    for (auto &[Alias, AI] : Reexports)
      SF[Alias] = AI.AliasFlags;
    return {std::move(SF), nullptr};
  }

  StringRef getName() const override { return "LazyReexportsManager::MU"; }

  void materialize(std::unique_ptr<MaterializationResponsibility> R) override {
    LRMgr.emitReentryTrampolines(std::move(R), std::move(Reexports));
  }

  void discard(const JITDylib &JD, const SymbolStringPtr &Name) override {
    Reexports.erase(Name);
  }

  LazyReexportsManager &LRMgr;
  SymbolAliasMap Reexports;
};

class LazyReexportsManager::Plugin : public ObjectLinkingLayer::Plugin {
public:
  void modifyPassConfig(MaterializationResponsibility &MR,
                        jitlink::LinkGraph &G,
                        jitlink::PassConfiguration &Config) override {}

  Error notifyFailed(MaterializationResponsibility &MR) override {
    return Error::success();
  }

  Error notifyRemovingResources(JITDylib &JD, ResourceKey K) override {
    return Error::success();
  }

  void notifyTransferringResources(JITDylib &JD, ResourceKey DstKey,
                                   ResourceKey SrcKey) override {}

private:
  std::mutex M;
};

LazyReexportsManager::Listener::~Listener() = default;

Expected<std::unique_ptr<LazyReexportsManager>>
LazyReexportsManager::Create(EmitTrampolinesFn EmitTrampolines,
                             RedirectableSymbolManager &RSMgr,
                             JITDylib &PlatformJD, Listener *L) {
  Error Err = Error::success();
  std::unique_ptr<LazyReexportsManager> LRM(new LazyReexportsManager(
      std::move(EmitTrampolines), RSMgr, PlatformJD, L, Err));
  if (Err)
    return std::move(Err);
  return std::move(LRM);
}

Error LazyReexportsManager::handleRemoveResources(JITDylib &JD, ResourceKey K) {
  return JD.getExecutionSession().runSessionLocked([&]() -> Error {
    auto I = KeyToReentryAddrs.find(K);
    if (I == KeyToReentryAddrs.end())
      return Error::success();

    auto &ReentryAddrs = I->second;
    for (auto &ReentryAddr : ReentryAddrs) {
      assert(CallThroughs.count(ReentryAddr) && "CallTrhough missing");
      CallThroughs.erase(ReentryAddr);
    }
    KeyToReentryAddrs.erase(I);
    return L ? L->onLazyReexportsRemoved(JD, K) : Error::success();
  });
}

void LazyReexportsManager::handleTransferResources(JITDylib &JD,
                                                   ResourceKey DstK,
                                                   ResourceKey SrcK) {
  auto I = KeyToReentryAddrs.find(SrcK);
  if (I != KeyToReentryAddrs.end()) {
    auto J = KeyToReentryAddrs.find(DstK);
    if (J == KeyToReentryAddrs.end()) {
      auto Tmp = std::move(I->second);
      KeyToReentryAddrs.erase(I);
      KeyToReentryAddrs[DstK] = std::move(Tmp);
    } else {
      auto &SrcAddrs = I->second;
      auto &DstAddrs = J->second;
      DstAddrs.insert(DstAddrs.end(), SrcAddrs.begin(), SrcAddrs.end());
      KeyToReentryAddrs.erase(I);
    }
    if (L)
      L->onLazyReexportsTransfered(JD, DstK, SrcK);
  }
}

LazyReexportsManager::LazyReexportsManager(EmitTrampolinesFn EmitTrampolines,
                                           RedirectableSymbolManager &RSMgr,
                                           JITDylib &PlatformJD, Listener *L,
                                           Error &Err)
    : ES(PlatformJD.getExecutionSession()),
      EmitTrampolines(std::move(EmitTrampolines)), RSMgr(RSMgr), L(L) {

  using namespace shared;

  ErrorAsOutParameter _(&Err);

  ExecutionSession::JITDispatchHandlerAssociationMap WFs;

  WFs[ES.intern("__orc_rt_resolve_tag")] =
      ES.wrapAsyncWithSPS<SPSExpected<SPSExecutorSymbolDef>(SPSExecutorAddr)>(
          this, &LazyReexportsManager::resolve);

  Err = ES.registerJITDispatchHandlers(PlatformJD, std::move(WFs));
}

std::unique_ptr<MaterializationUnit>
LazyReexportsManager::createLazyReexports(SymbolAliasMap Reexports) {
  return std::make_unique<MU>(*this, std::move(Reexports));
}

void LazyReexportsManager::emitReentryTrampolines(
    std::unique_ptr<MaterializationResponsibility> MR,
    SymbolAliasMap Reexports) {
  size_t NumTrampolines = Reexports.size();
  auto RT = MR->getResourceTracker();
  EmitTrampolines(
      std::move(RT), NumTrampolines,
      [this, MR = std::move(MR), Reexports = std::move(Reexports)](
          Expected<std::vector<ExecutorSymbolDef>> ReentryPoints) mutable {
        emitRedirectableSymbols(std::move(MR), std::move(Reexports),
                                std::move(ReentryPoints));
      });
}

void LazyReexportsManager::emitRedirectableSymbols(
    std::unique_ptr<MaterializationResponsibility> MR, SymbolAliasMap Reexports,
    Expected<std::vector<ExecutorSymbolDef>> ReentryPoints) {

  if (!ReentryPoints) {
    MR->getExecutionSession().reportError(ReentryPoints.takeError());
    MR->failMaterialization();
    return;
  }

  assert(Reexports.size() == ReentryPoints->size() &&
         "Number of reentry points doesn't match number of reexports");

  // Bind entry points to names.
  SymbolMap Redirs;
  size_t I = 0;
  for (auto &[Name, AI] : Reexports)
    Redirs[Name] = {(*ReentryPoints)[I++].getAddress(), AI.AliasFlags};

  I = 0;
  if (!Reexports.empty()) {
    if (auto Err = MR->withResourceKeyDo([&](ResourceKey K) {
          auto &JD = MR->getTargetJITDylib();
          auto &ReentryAddrsForK = KeyToReentryAddrs[K];
          for (auto &[Name, AI] : Reexports) {
            const auto &ReentryPoint = (*ReentryPoints)[I++];
            CallThroughs[ReentryPoint.getAddress()] = {&JD, Name, AI.Aliasee};
            ReentryAddrsForK.push_back(ReentryPoint.getAddress());
          }
          if (L)
            L->onLazyReexportsCreated(JD, K, Reexports);
        })) {
      MR->getExecutionSession().reportError(std::move(Err));
      MR->failMaterialization();
      return;
    }
  }

  RSMgr.emitRedirectableSymbols(std::move(MR), std::move(Redirs));
}

void LazyReexportsManager::resolve(ResolveSendResultFn SendResult,
                                   ExecutorAddr ReentryStubAddr) {

  CallThroughInfo LandingInfo;

  ES.runSessionLocked([&]() {
    auto I = CallThroughs.find(ReentryStubAddr);
    if (I == CallThroughs.end())
      return SendResult(make_error<StringError>(
          "Reentry address " + formatv("{0:x}", ReentryStubAddr) +
              " not registered",
          inconvertibleErrorCode()));
    LandingInfo = I->second;
  });

  if (L)
    L->onLazyReexportCalled(LandingInfo);

  SymbolInstance LandingSym(LandingInfo.JD, std::move(LandingInfo.BodyName));
  LandingSym.lookupAsync([this, JD = std::move(LandingInfo.JD),
                          ReentryName = std::move(LandingInfo.Name),
                          SendResult = std::move(SendResult)](
                             Expected<ExecutorSymbolDef> Result) mutable {
    if (Result) {
      // FIXME: Make RedirectionManager operations async, then use the async
      //        APIs here.
      if (auto Err = RSMgr.redirect(*JD, ReentryName, *Result))
        SendResult(std::move(Err));
      else
        SendResult(std::move(Result));
    } else
      SendResult(std::move(Result));
  });
}

class SimpleLazyReexportsSpeculator::SpeculateTask : public IdleTask {
public:
  SpeculateTask(std::weak_ptr<SimpleLazyReexportsSpeculator> Speculator)
      : Speculator(std::move(Speculator)) {}

  void printDescription(raw_ostream &OS) override {
    OS << "Speculative Lookup Task";
  }

  void run() override {
    if (auto S = Speculator.lock())
      S->doNextSpeculativeLookup();
  }

private:
  std::weak_ptr<SimpleLazyReexportsSpeculator> Speculator;
};

SimpleLazyReexportsSpeculator::~SimpleLazyReexportsSpeculator() {
  for (auto &[JD, _] : LazyReexports)
    JITDylibSP(JD)->Release();
}

void SimpleLazyReexportsSpeculator::onLazyReexportsCreated(
    JITDylib &JD, ResourceKey K, const SymbolAliasMap &Reexports) {
  if (!LazyReexports.count(&JD))
    JD.Retain();
  auto &BodiesVec = LazyReexports[&JD][K];
  for (auto &[Name, AI] : Reexports)
    BodiesVec.push_back(AI.Aliasee);
  if (!SpeculateTaskActive) {
    SpeculateTaskActive = true;
    ES.dispatchTask(std::make_unique<SpeculateTask>(WeakThis));
  }
}

void SimpleLazyReexportsSpeculator::onLazyReexportsTransfered(
    JITDylib &JD, ResourceKey DstK, ResourceKey SrcK) {

  auto I = LazyReexports.find(&JD);
  if (I == LazyReexports.end())
    return;

  auto &MapForJD = I->second;
  auto J = MapForJD.find(SrcK);
  if (J == MapForJD.end())
    return;

  // We have something to transfer.
  auto K = MapForJD.find(DstK);
  if (K == MapForJD.end()) {
    auto Tmp = std::move(J->second);
    MapForJD.erase(J);
    MapForJD[DstK] = std::move(Tmp);
  } else {
    auto &SrcNames = J->second;
    auto &DstNames = K->second;
    DstNames.insert(DstNames.end(), SrcNames.begin(), SrcNames.end());
    MapForJD.erase(J);
  }
}

Error SimpleLazyReexportsSpeculator::onLazyReexportsRemoved(JITDylib &JD,
                                                            ResourceKey K) {

  auto I = LazyReexports.find(&JD);
  if (I == LazyReexports.end())
    return Error::success();

  auto &MapForJD = I->second;
  MapForJD.erase(K);

  if (MapForJD.empty()) {
    LazyReexports.erase(I);
    JD.Release();
  }

  return Error::success();
}

void SimpleLazyReexportsSpeculator::onLazyReexportCalled(
    const CallThroughInfo &CTI) {
  if (RecordExec)
    RecordExec(CTI);
}

void SimpleLazyReexportsSpeculator::addSpeculationSuggestions(
    std::vector<std::pair<std::string, SymbolStringPtr>> NewSuggestions) {
  ES.runSessionLocked([&]() {
    for (auto &[JDName, SymbolName] : NewSuggestions)
      SpeculateSuggestions.push_back(
          {std::move(JDName), std::move(SymbolName)});
  });
}

bool SimpleLazyReexportsSpeculator::doNextSpeculativeLookup() {
  // Use existing speculation queue if available, otherwise take the next
  // element from LazyReexports.
  JITDylibSP SpeculateJD = nullptr;
  SymbolStringPtr SpeculateFn;

  auto SpeculateAgain = ES.runSessionLocked([&]() {
    while (!SpeculateSuggestions.empty()) {
      auto [JDName, SymbolName] = std::move(SpeculateSuggestions.front());
      SpeculateSuggestions.pop_front();

      if (auto *JD = ES.getJITDylibByName(JDName)) {
        SpeculateJD = JD;
        SpeculateFn = std::move(SymbolName);
        break;
      }
    }

    if (!SpeculateJD) {
      assert(!LazyReexports.empty() && "LazyReexports map is empty");
      auto LRItr =
          std::next(LazyReexports.begin(), rand() % LazyReexports.size());
      auto &[JD, KeyToFnBodies] = *LRItr;

      assert(!KeyToFnBodies.empty() && "Key to function bodies map empty");
      auto KeyToFnBodiesItr =
          std::next(KeyToFnBodies.begin(), rand() % KeyToFnBodies.size());
      auto &[Key, FnBodies] = *KeyToFnBodiesItr;

      assert(!FnBodies.empty() && "Function bodies list empty");
      auto FnBodyItr = std::next(FnBodies.begin(), rand() % FnBodies.size());

      SpeculateJD = JITDylibSP(JD);
      SpeculateFn = std::move(*FnBodyItr);

      FnBodies.erase(FnBodyItr);
      if (FnBodies.empty()) {
        KeyToFnBodies.erase(KeyToFnBodiesItr);
        if (KeyToFnBodies.empty()) {
          LRItr->first->Release();
          LazyReexports.erase(LRItr);
        }
      }
    }

    SpeculateTaskActive =
        !SpeculateSuggestions.empty() || !LazyReexports.empty();
    return SpeculateTaskActive;
  });

  LLVM_DEBUG({
    dbgs() << "Issuing speculative lookup for ( " << SpeculateJD->getName()
           << ", " << SpeculateFn << " )...\n";
  });

  ES.lookup(
      LookupKind::Static, makeJITDylibSearchOrder(SpeculateJD.get()),
      {{std::move(SpeculateFn), SymbolLookupFlags::WeaklyReferencedSymbol}},
      SymbolState::Ready,
      [](Expected<SymbolMap> Result) { consumeError(Result.takeError()); },
      NoDependenciesToRegister);

  if (SpeculateAgain)
    ES.dispatchTask(std::make_unique<SpeculateTask>(WeakThis));

  return false;
}

} // End namespace orc.
} // End namespace llvm.
