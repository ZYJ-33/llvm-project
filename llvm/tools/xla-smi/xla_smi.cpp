//
// Created by 郑毓嘉 on 2023/8/28.

#include "llvm/Support/InitLLVM.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/WithColor.h"

using namespace llvm;
static cl::opt<char>
    OptLevel("O",
             cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                      "(default = '-O0')"),
             cl::Prefix, cl::init('0'));

static cl::opt<std::string>
    mcpu("mcpu",
         cl::desc("mcpu"),
         cl::Prefix, cl::init("none"));

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string>
    OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));


[[noreturn]] static void reportError(Twine Msg, StringRef Filename = "") {
  SmallString<256> Prefix;
  if (!Filename.empty()) {
    if (Filename == "-")
      Filename = "<stdin>";
    ("'" + Twine(Filename) + "': ").toStringRef(Prefix);
  }
  WithColor::error(errs(), "llc") << Prefix << Msg << "\n";
  exit(1);
}


void InitializePasses(llvm::PassRegistry* pass_registry) {
  llvm::initializeCore(*pass_registry);
  llvm::initializeCodeGen(*pass_registry);
  llvm::initializeScalarOpts(*pass_registry);
  llvm::initializeVectorization(*pass_registry);
  llvm::initializeIPO(*pass_registry);
  llvm::initializeAnalysis(*pass_registry);
  llvm::initializeTransformUtils(*pass_registry);
  llvm::initializeInstCombine(*pass_registry);
  llvm::initializeTarget(*pass_registry);
  llvm::initializeCodeGenPreparePass(*pass_registry);
}

static int compileModule(char **argv, LLVMContext &Context) {
  std::unique_ptr<Module> M;
  Triple TheTriple("nvptx64-unknown-unknown");
  std::string arch = "nvptx64";
  std::string sm_name = mcpu;
  SMDiagnostic Err;
  const Target *TheTarget = nullptr;
  std::unique_ptr<TargetMachine> Target;

  CodeGenOpt::Level OLvl;
  if (auto Level = CodeGenOpt::parseLevel(OptLevel)) {
    OLvl = *Level;
  } else {
    WithColor::error(errs(), argv[0]) << "invalid optimization level.\n";
    return 1;
  }


  auto checkDataLayout = [&](StringRef TargetTriple,
                           StringRef DataLayout)-> std::optional<std::string>
  {
      TheTriple = Triple(TargetTriple.str());
      std::string Error;
      TheTarget =
          TargetRegistry::lookupTarget(arch, TheTriple, Error);
      if (!TheTarget) {
        WithColor::error(errs(), argv[0]) << Error;
        exit(1);
      }

      return std::optional<std::string>();
  };


  M = parseIRFile(InputFilename, Err, Context,
                  ParserCallbacks(checkDataLayout));
  if (!M) {
    Err.print(argv[0], WithColor::error(errs(), argv[0]));
    return 1;
  }

}

int main(int argc, char** argv)
{
  InitLLVM X(argc, argv);
  LLVMContext Context;
  LLVMInitializeNVPTXTargetInfo();
  LLVMInitializeNVPTXTarget();
  LLVMInitializeNVPTXTargetMC();
  LLVMInitializeNVPTXAsmPrinter();

  PassRegistry *pass_registry = PassRegistry::getPassRegistry();
  InitializePasses(pass_registry);

  cl::ParseCommandLineOptions(argc, argv, "xla-backend simulator\n");

  /*
  std::string CPUStr = codegen::getCPUStr(),
              FeaturesStr = codegen::getFeaturesStr();
  */
  outs()<<codegen::getMArch()<<"\n";

  compileModule(argv, Context);

}


