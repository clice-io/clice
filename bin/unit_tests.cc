#include "Test/Test.h"
#include "Support/GlobPattern.h"
#include "Support/Logging.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Signals.h"

using namespace clice;
using namespace clice::testing;

namespace {

namespace cl = llvm::cl;

cl::OptionCategory unittest_category("clice Unittest Options");

cl::opt<std::string> test_dir{
    "test-dir",
    cl::desc("Specify the test source directory path"),
    cl::value_desc("path"),
    cl::Required,
    cl::cat(unittest_category),
};

cl::opt<std::string> test_filter{
    "test-filter",
    cl::desc("A glob pattern to run subset of tests"),
    cl::cat(unittest_category),
};

cl::opt<bool> enable_example{
    "enable-example",
    cl::init(false),
    cl::cat(unittest_category),
};

}  // namespace

int main(int argc, const char* argv[]) {
    llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
    llvm::cl::HideUnrelatedOptions(unittest_category);
    llvm::cl::ParseCommandLineOptions(argc, argv, "clice test\n");

    logging::stderr_logger("clice", logging::options);

    if(auto result = fs::init_resource_dir(argv[0]); !result) {
        std::println("Failed to get resource directory, because {}", result.error());
        return 1;
    }

    using namespace clice::testing;
    return Runner2::instance().run_tests(test_filter);
}
