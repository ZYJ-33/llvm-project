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
#include <fstream>

using namespace llvm;
static codegen::RegisterCodeGenFlags CGF;
const std::string libdevice_path = "./libdevice.10.ll";
const int kDefaultInlineThreshold = 1100;


static cl::opt<char>
    OptLevel("O",
             cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                      "(default = '-O0')"),
             cl::Prefix, cl::init('0'));

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

namespace {
void write_to_file(const std::string &filename, const std::string &ptx) {
  std::ofstream ofs(filename, std::ios::out | std::ios::trunc);
  if (ofs.is_open()) {
    ofs << ptx;
    ofs.close();
  } else {
    errs() << "fail to open " << filename << "\n";
    exit(1);
  }
}

std::string EmitModuleToPTX(Module *module, TargetMachine *target_machine) {
  std::string ptx;
  llvm::raw_string_ostream stream(ptx);
  llvm::buffer_ostream pstream(stream);
  llvm::legacy::PassManager pm;
  pm.add(new llvm::TargetLibraryInfoWrapperPass(
      llvm::Triple(module->getTargetTriple())));
  target_machine->addPassesToEmitFile(pm, pstream, nullptr,
                                      llvm::CGFT_AssemblyFile);
  pm.run(*module);
  return ptx;
}

void FeedLLVMWithFlags(const std::vector<std::string> &cl_opts) {
  std::vector<const char *> fake_argv = {""};
  for (const std::string &cl_opt : cl_opts) {
    fake_argv.push_back(cl_opt.c_str());
  }
  llvm::cl::ParseCommandLineOptions(fake_argv.size(), &fake_argv[0]);
}

void InitializePasses(llvm::PassRegistry *pass_registry) {
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

bool CouldNeedDeviceBitcode(const llvm::Module &module) {
  for (const llvm::Function &function : module.functions()) {
    // The list of prefixes should be in sync with library functions used in
    // target_util.cc.
    if (!function.isIntrinsic() && function.isDeclaration() &&
        (function.getName().startswith("__nv_") ||
         function.getName().startswith("__ocml_") ||
         function.getName().startswith("__ockl_"))) {
      return true;
    }
  }
  return false;
}

static void DieWithSMDiagnosticError(llvm::SMDiagnostic *diagnostic) {
  errs() << diagnostic->getFilename().str() << ":" << diagnostic->getLineNo()
         << ":" << diagnostic->getColumnNo() << ": "
         << diagnostic->getMessage().str();
  exit(1);
}

std::unique_ptr<llvm::Module> LoadIRModule(const std::string &filename,
                                           llvm::LLVMContext *llvm_context) {
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> module =
      llvm::getLazyIRFileModule(filename, Err, *llvm_context,
                                /*ShouldLazyLoadMetadata=*/true);
  if (!module)
    DieWithSMDiagnosticError(&Err);
  return module;
}

void LinkWithBitcodeVector(
    llvm::Module *module, const std::vector<std::string> &bitcode_path_vector) {
  Linker linker(*module);
  auto internalize_callback = [](llvm::Module &M,
                                 const llvm::StringSet<> &GVS) {
    internalizeModule(M, [&GVS](const llvm::GlobalValue &GV) {
      return !GV.hasName() || (GVS.count(GV.getName()) == 0);
    });
  };
  for (auto &bitcode_path : bitcode_path_vector) {
    std::unique_ptr<llvm::Module> bitcode_module =
        LoadIRModule(bitcode_path, &module->getContext());
    bitcode_module->setDataLayout(module->getDataLayout());

    if (linker.linkInModule(std::move(bitcode_module),
                            Linker::Flags::LinkOnlyNeeded,
                            internalize_callback)) {
      errs() << "err happen when linking " + bitcode_path + "\n";
      exit(1);
    }
  }
}

void LinkLibdeviceIfNecessary(llvm::Module *module,
                              const std::string &libdevice_path) {
  if (!CouldNeedDeviceBitcode(*module))
    return;
  LinkWithBitcodeVector(module, {libdevice_path});
}

void LinkAndOptimizeModule(Module *module, llvm::TargetMachine *target_machine,
                           int inline_threshold) {
  LinkLibdeviceIfNecessary(module, libdevice_path);

  /*

   module->addModuleFlag(llvm::Module::Override, "nvvm-reflect-ftz",
                debug_options.xla_gpu_ftz());
  // If ftz is enabled, set it as an attribute on every function in the module.
  if (debug_options.xla_gpu_ftz()) {
  for (llvm::Function& fn : *module) {
    fn.addFnAttr("denormal-fp-math-f32", "preserve-sign");
  }
  }
  */
  // todo: figure out what to do with this

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

  fam.registerPass([&] { return target_machine->getTargetIRAnalysis(); });

  llvm::PipelineTuningOptions pto;
  pto.SLPVectorization = true;
  pto.InlinerThreshold = inline_threshold;

  llvm::PassInstrumentationCallbacks pic;

  llvm::StandardInstrumentations si(module->getContext(), false);
  si.registerCallbacks(pic, &mam);

  llvm::PassBuilder pb(target_machine, pto, std::nullopt, &pic);
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  llvm::OptimizationLevel ol;
  switch (OptLevel) {
  case '0':
    ol = llvm::OptimizationLevel::O0;
    break;
  case '1':
    ol = llvm::OptimizationLevel::O1;
    break;
  case '2':
    ol = llvm::OptimizationLevel::O2;
    break;
  case '3':
    ol = llvm::OptimizationLevel::O3;
    break;
  }
  llvm::ModulePassManager mpm;
  mpm.addPass(llvm::VerifierPass());
  if (ol == llvm::OptimizationLevel::O0) {
    mpm.addPass(pb.buildO0DefaultPipeline(ol));
  } else {
    mpm.addPass(pb.buildPerModuleDefaultPipeline(ol));
  }
  mpm.addPass(llvm::VerifierPass());

  mpm.run(*module, mam);
}

static int compileModule(char **argv, LLVMContext &Context) {
  const Target *TheTarget = nullptr;
  Triple TheTriple;
  std::unique_ptr<TargetMachine> Target;
  std::unique_ptr<Module> M;

  std::optional<Reloc::Model> RM = codegen::getExplicitRelocModel();
  std::optional<CodeModel::Model> CM = codegen::getExplicitCodeModel();
  std::string CPUStr = codegen::getCPUStr(); // this is sm_num
  std::string FeaturesStr =
      codegen::getFeaturesStr(); // todo:shoube set to ptx version?
  std::string arch = "nvptx64";

  TargetOptions Options =
      codegen::InitTargetOptionsFromCodeGenFlags(llvm::Triple());

  CodeGenOpt::Level OLvl;
  if (auto Level = CodeGenOpt::parseLevel(OptLevel))
    OLvl = *Level;
  else {
    WithColor::error(errs(), argv[0]) << "invalid optimization level.\n";
    return 1;
  }

  auto SetDataLayout = [&](StringRef TargetTriple,
                           StringRef DataLayout) -> std::optional<std::string> {
    TheTriple = Triple(TargetTriple.str());
    std::string Error;
    TheTarget = TargetRegistry::lookupTarget(arch, TheTriple, Error);
    if (!TheTarget) {
      WithColor::error(errs(), argv[0]) << Error;
      exit(1);
    }
    Target = std::unique_ptr<TargetMachine>(TheTarget->createTargetMachine(
        TheTriple.getTriple(), CPUStr, FeaturesStr, Options, RM, CM, OLvl));
    assert(Target && "Could not allocate target machine!");

    return Target->createDataLayout().getStringRepresentation();
  };
  SMDiagnostic Err;
  M = parseIRFile(InputFilename, Err, Context, ParserCallbacks(SetDataLayout));

  if (!M) {
    Err.print(argv[0], WithColor::error(errs(), argv[0]));
    return 1;
  }

  LinkAndOptimizeModule(M.get(), Target.get(), kDefaultInlineThreshold);
  std::string ptx = EmitModuleToPTX(M.get(), Target.get());

  std::string output_file;
  if (OutputFilename != "")
    output_file = OutputFilename;
  else
    output_file = InputFilename.substr(0, InputFilename.size() - 2) + "ptx";

  write_to_file(output_file, ptx);
  return 1;
}
}

int main(int argc, char** argv)
{
  InitLLVM X(argc, argv);
  LLVMContext Context;
  FeedLLVMWithFlags({"-bonus-inst-threshold=2"});
  FeedLLVMWithFlags({"-nvptx-prec-divf32=1"});
  FeedLLVMWithFlags({
      "-slp-vectorize-hor=false",
      "-slp-max-reg-size=32",
  });

  LLVMInitializeNVPTXTargetInfo();
  LLVMInitializeNVPTXTarget();
  LLVMInitializeNVPTXTargetMC();
  LLVMInitializeNVPTXAsmPrinter();

  PassRegistry *pass_registry = PassRegistry::getPassRegistry();
  InitializePasses(pass_registry);

  cl::ParseCommandLineOptions(argc, argv, "xla-backend simulator\n");

  compileModule(argv, Context);
}


