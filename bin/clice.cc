#include "Server/Version.h"
#include "Server/Server.h"
#include "Support/Logging.h"
#include "Support/Format.h"

#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Process.h"
#include "llvm/ADT/StringSwitch.h"

namespace cl = llvm::cl;
using namespace clice;

namespace {

static cl::OptionCategory category("clice options");

enum class Mode {
    Pipe,
    Socket,
    Indexer,
};

cl::opt<Mode> mode{
    "mode",
    cl::cat(category),
    cl::value_desc("string"),
    cl::init(Mode::Pipe),
    cl::values(clEnumValN(Mode::Pipe, "pipe", "pipe mode, clice will listen on stdio"),
               clEnumValN(Mode::Socket, "socket", "socket mode, clice will listen on host:port")),
    ///  clEnumValN(Mode::Indexer, "indexer", "indexer mode, to implement")
    cl::desc("The mode of clice, default is pipe, socket is usually used for debugging"),
};

cl::opt<std::string> host{
    "host",
    cl::cat(category),
    cl::value_desc("string"),
    cl::init("127.0.0.1"),
    cl::desc("The host to connect to (default: 127.0.0.1)"),
};

cl::opt<unsigned int> port{
    "port",
    cl::cat(category),
    cl::value_desc("unsigned int"),
    cl::init(50051),
    cl::desc("The port to connect to"),
};

cl::opt<std::string> resource_dir{
    "resource-dir",
    cl::cat(category),
    cl::value_desc("path"),
    cl::desc(R"(The path of the clang resource directory, default is "../../lib/clang/version")"),
};

cl::opt<std::string> log_color{
    "log-color",
    cl::cat(category),
    cl::value_desc("always|auto|never"),
    cl::init("auto"),
    cl::desc("When to use terminal colors, default is auto"),
};

cl::opt<logging::Level> log_level{
    "log-level",
    cl::cat(category),
    cl::value_desc("trace|debug|info|warn|fatal"),
    cl::init(logging::Level::info),
    cl::values(clEnumValN(logging::Level::trace, "trace", ""),
               clEnumValN(logging::Level::debug, "debug", ""),
               clEnumValN(logging::Level::info, "info", ""),
               clEnumValN(logging::Level::warn, "warn", ""),
               clEnumValN(logging::Level::err, "fatal", "")),
    cl::desc("The log level, default is info"),
};

void init_log() {
    using namespace logging;
    if(auto color_mode = llvm::StringRef{log_color}; !color_mode.compare("never")) {
        options.color = false;
    } else if(!color_mode.compare("always")) {
        options.color = true;
    } else {
        // Auto mode
        options.color = llvm::sys::Process::StandardErrIsDisplayed();
    }
    options.level = log_level;

    logging::create_stderr_logger("clice", logging::options);
}

/// Check the command line arguments and initialize the clice.
bool check_arguments(int argc, const char** argv) {
    /// Hide unrelated options.
    cl::HideUnrelatedOptions(category);

    // Set version printer and parse command line options
    cl::SetVersionPrinter([](llvm::raw_ostream& os) {
        os << std::format("clice version: {}\nllvm version: {}\n",
                          clice::config::version,
                          clice::config::llvm_version);
    });
    cl::ParseCommandLineOptions(argc,
                                argv,
                                "clice is a new generation of language server for C/C++");

    init_log();

    for(int i = 0; i < argc; ++i) {
        logging::info("argv[{}] = {}", i, argv[i]);
    }

    // Initialize resource directory
    if(resource_dir.empty()) {
        logging::info("No resource directory specified, using default resource directory");
        // Try to initialize default resource directory
        if(auto result = fs::init_resource_dir(argv[0]); !result) {
            logging::warn("Cannot find default resource directory, because {}", result.error());
            return false;
        }
    } else {
        // Set and check the specified resource directory
        fs::resource_dir = resource_dir.getValue();
        if(fs::exists(fs::resource_dir)) {
            logging::info("Resource directory found: {}", fs::resource_dir);
        } else {
            logging::warn("Resource directory not found: {}", fs::resource_dir);
            return false;
        }
    }

    return true;
}

}  // namespace

int main(int argc, const char** argv) {
    llvm::InitLLVM guard(argc, argv);
    llvm::setBugReportMsg(
        "Please report bugs to https://github.com/clice-io/clice/issues and include the crash backtrace");

    if(!check_arguments(argc, argv)) {
        return 1;
    }

    async::init();

    /// The global server instance.
    static Server instance;
    auto loop = [&](json::Value value) -> async::Task<> {
        co_await instance.on_receive(value);
    };

    switch(mode) {
        case Mode::Pipe: {
            async::net::listen(loop);
            logging::info("Server starts listening on stdin/stdout");
            break;
        }

        case Mode::Socket: {
            async::net::listen(host.c_str(), port, loop);
            logging::info("Server starts listening on {}:{}", host.getValue(), port.getValue());
            break;
        }

        case Mode::Indexer: {
            /// TODO:
            break;
        }
    }

    async::run();

    logging::info("clice exit normally!");

    return 0;
}
