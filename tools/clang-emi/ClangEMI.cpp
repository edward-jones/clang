//===--- ClangEMI.cpp - Clang-based Equivalence Modulo Input tool ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ProfileData/GCOV.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <string>
#include <system_error>

using namespace clang;
using namespace tooling;
namespace cl = llvm::cl;


static cl::opt<bool> InPlace("i", cl::desc("Inplace edit <file>s"),
                             cl::cat(cl::GeneralCategory),
                             cl::sub(*cl::AllSubCommands));


namespace {

class ClangEMI {
public:
  using TUCallbackType = std::function<void(ASTContext &)>;

  void callback(ASTContext &AST) {
  }

  llvm::Expected<std::unique_ptr<FrontendActionFactory>>
  getFrontendActionFactory() {
    class ToolASTConsumer : public ASTConsumer {
    public:
      TUCallbackType Callback;
      ToolASTConsumer(TUCallbackType Callback)
          : Callback(std::move(Callback)) {}

      void HandleTranslationUnit(ASTContext &Context) override {
        Callback(Context);
      }
    };

    class ToolASTAction : public ASTFrontendAction {
    public:
      explicit ToolASTAction(TUCallbackType Callback)
          : Callback(std::move(Callback)) {}

    protected:
      std::unique_ptr<clang::ASTConsumer>
      CreateASTConsumer(clang::CompilerInstance &compiler,
                        StringRef /* dummy */) override {
        std::unique_ptr<clang::ASTConsumer> Consumer{
            new ToolASTConsumer(Callback)};
        return Consumer;
      }

    private:
      TUCallbackType Callback;
    };

    class ToolActionFactory : public FrontendActionFactory {
    public:
      ToolActionFactory(TUCallbackType Callback)
          : Callback(std::move(Callback)) {}

      FrontendAction *create() override { return new ToolASTAction(Callback); }

    private:
      TUCallbackType Callback;
    };

    return llvm::make_unique<ToolActionFactory>(
        [this](ASTContext &AST) { return callback(AST); });
  }
};


ast_matchers::StatementMatcher IfMatcher = ast_matchers::ifStmt().bind("if");

class IfDeleter : public ast_matchers::MatchFinder::MatchCallback {
public:
  explicit IfDeleter(AtomicChanges *Changes,
                     llvm::StringMap<std::set<uint32_t>> FileLines)
      : ast_matchers::MatchFinder::MatchCallback(), FileLinesTouched(FileLines),
        SourceChanges(Changes) {}

  virtual void run(const ast_matchers::MatchFinder::MatchResult &Result) {
    const SourceManager *SM = Result.SourceManager;
    if (const IfStmt *If = Result.Nodes.getNodeAs<clang::IfStmt>("if")) {
      // TODO: Check if the source range of the if statement appear in the
      // coverage data.
      const Stmt *Then = If->getThen();

      auto ThenRange = If->getThen()->getSourceRange();
      std::pair<FileID, unsigned> ThenStart;
      std::pair<FileID, unsigned> ThenEnd;
      if (const auto *CompoundThen = dyn_cast<CompoundStmt>(Then)) {
        ThenStart = SM->getDecomposedLoc(CompoundThen->body_front()->getBeginLoc());
        ThenEnd = SM->getDecomposedLoc(CompoundThen->body_back()->getEndLoc());
      } else {
        ThenStart = SM->getDecomposedLoc(ThenRange.getBegin());
        ThenEnd   = SM->getDecomposedLoc(ThenRange.getEnd());
      }
      auto ThenStartLine = SM->getLineNumber(ThenStart.first, ThenStart.second);
      auto ThenEndLine   = SM->getLineNumber(ThenEnd.first, ThenEnd.second);

      // Check whether anything in the range [IfStartLine, IfEndLine] is
      // touched in the coverage data
      StringRef SourceFile = SM->getBufferName(If->getIfLoc());
      llvm::outs() << SourceFile << "\n";
      std::set<uint32_t> LinesTouched = FileLinesTouched[SourceFile];
      for (auto i = ThenStartLine; i <= ThenEndLine; ++i) {
        llvm::outs() << std::to_string(i) << "\n";
        if (LinesTouched.count(i)) {
          llvm::outs() << "Line touched\n";
          return;
        }
      }

      // For now we just delete all if statements by setting up a Refactor
      // to delete the source range
      AtomicChange Change(*SM, ThenRange.getBegin());
      llvm::Error Err = Change.replace(*SM,
                                       CharSourceRange::getTokenRange(ThenRange),
                                       "{}");
      if (Err) {
        llvm::errs() << Err << "\n";
        return;
      }
      SourceChanges->push_back(std::move(Change));
    }
  }

private:
  llvm::StringMap<std::set<uint32_t>> FileLinesTouched;
  AtomicChanges *SourceChanges;
};

} // end anonymous namespace

int main(int argc, const char **argv) {
  CommonOptionsParser Options(
    argc, argv, cl::GeneralCategory, cl::ZeroOrMore,
    "Clang-based Equivalence Modulo Input tool for C, C++ and Objective-C");

  llvm::outs() << "Reading GCOV coverage\n";

  llvm::StringMap<std::set<uint32_t>> FileLinesTouched;
  llvm::StringMap<std::string> FileToEffectivePath;

  for (const auto SourceFile : Options.getSourcePathList()) {
    SmallString<128> EffectivePath(SourceFile);
    if (std::error_code EC = llvm::sys::fs::make_absolute(EffectivePath)) {
      llvm::errs() << EC.message() << ": " << SourceFile << "\n";
      return -1;
    }
    FileToEffectivePath[SourceFile] = EffectivePath.str();
  }

  for (const auto SourceFile : Options.getSourcePathList()) {
    llvm::outs() << SourceFile << "\n";
    SmallString<128> CoverageFileStem =
        llvm::sys::path::parent_path(SourceFile);
    llvm::sys::path::append(CoverageFileStem,
                            llvm::sys::path::stem(SourceFile));
    std::string GCNO = std::string(CoverageFileStem.str()) + ".gcno";
    std::string GCDA = std::string(CoverageFileStem.str()) + ".gcda";

    llvm::GCOVFile GF;
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> GCNO_Buff =
        llvm::MemoryBuffer::getFileOrSTDIN(GCNO);
    if (std::error_code EC = GCNO_Buff.getError()) {
      llvm::errs() << GCNO << ": " << EC.message() << "\n";
      return -1;
    }
    llvm::GCOVBuffer GCNO_GB(GCNO_Buff.get().get());
    if (!GF.readGCNO(GCNO_GB)) {
      llvm::errs() << "Invalid .gcno File!\n";
      return -1;
    }
  
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> GCDA_Buff =
        llvm::MemoryBuffer::getFileOrSTDIN(GCDA);
    if (std::error_code EC = GCDA_Buff.getError()) {
      if (EC != llvm::errc::no_such_file_or_directory) {
        llvm::errs() << GCDA << ": " << EC.message() << "\n";
        return -1;
      }
      // Clear to filename to make it clear we didn't read anything.
      GCDA = "-";
    } else {
      llvm::GCOVBuffer GCDA_GB(GCDA_Buff.get().get());
      if (!GF.readGCDA(GCDA_GB)) {
        llvm::errs() << "Invalid .gcda File!\n";
        return -1;
      }
    }

    llvm::GCOV::Options GCOVOpts(false, false, false, false, false, false,
                                 false, false);
    llvm::FileInfo FI(GCOVOpts);
    GF.collectLineCounts(FI);

    const llvm::FileInfo::LineData &Line = FI.getLineInfo()[SourceFile];
    for (uint32_t LineIndex = 0; LineIndex < Line.LastLine; ++LineIndex) {
      auto BlocksIt = Line.Blocks.find(LineIndex);
      if (BlocksIt != Line.Blocks.end()) {
        const auto &Blocks = BlocksIt->second;

        for (const auto *Block : Blocks) {
          if (Block->getCount())
            FileLinesTouched[FileToEffectivePath[SourceFile]].insert(LineIndex + 1);
        }
      }
    }

    llvm::outs() << FileToEffectivePath[SourceFile] << "\n";
    for (auto line : FileLinesTouched[FileToEffectivePath[SourceFile]]) {
      llvm::outs() << std::to_string(line) << "\n";
    }
  }

  llvm::outs() << "Deleting if statements\n";

  AtomicChanges SourceChanges;

  IfDeleter Deleter(&SourceChanges, FileLinesTouched);
  ast_matchers::MatchFinder Finder;
  Finder.addMatcher(IfMatcher, &Deleter);

  ClangTool Tool(Options.getCompilations(), Options.getSourcePathList());
  bool Failed = false;
  if (Tool.run(newFrontendActionFactory(&Finder).get()) != 0) {
    llvm::errs() << "Failed to delete if statments\n";
    Failed = true;
    return Failed;
  }

  llvm::outs() << "Applying Source Changes\n";

  std::set<std::string> Files;
  for (const auto &Change : SourceChanges)
    Files.insert(Change.getFilePath());

  tooling::ApplyChangesSpec Spec;
  Spec.Cleanup = false;
  for (const auto &File : Files) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> BufferErr =
        llvm::MemoryBuffer::getFile(File);
    if (!BufferErr) {
      llvm::errs() << "error: failed to open " << File << " for rewriting\n";
      return true;
    }
    auto Result = tooling::applyAtomicChanges(File, (*BufferErr)->getBuffer(),
                                              SourceChanges, Spec);
    if (!Result) {
      llvm::errs() << llvm::toString(Result.takeError());
      return true;
    }

    if (InPlace) {
      std::error_code EC;
      llvm::raw_fd_ostream OS(File, EC, llvm::sys::fs::F_Text);
      if (EC) {
        llvm::errs() << EC.message() << "\n";
        return true;
      }
      OS << *Result;
      continue;
    }

    llvm::outs() << *Result;
  }

  return Failed;
}
