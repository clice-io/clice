#include "Server/Server.h"
#include "Server/Version.h"
#include "Support/Format.h"
#include "Support/Logging.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Process.h"

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

cl::opt<logging::ColorMode> log_color{
    "log-color",
    cl::cat(category),
    cl::value_desc("always|auto|never"),
    cl::init(logging::ColorMode::automatic),
    cl::values(clEnumValN(logging::ColorMode::automatic, "auto", ""),
               clEnumValN(logging::ColorMode::always, "always", ""),
               clEnumValN(logging::ColorMode::never, "never", "")),
    cl::desc("When to use terminal colors, default is auto"),
};

cl::opt<logging::Level> log_level{
    "log-level",
    cl::cat(category),
    cl::value_desc("trace|debug|info|warn|error"),
    cl::init(logging::Level::info),
    cl::values(clEnumValN(logging::Level::trace, "trace", ""),
               clEnumValN(logging::Level::debug, "debug", ""),
               clEnumValN(logging::Level::info, "info", ""),
               clEnumValN(logging::Level::warn, "warn", ""),
               clEnumValN(logging::Level::err, "error", ""),
               clEnumValN(logging::Level::off, "off", "")),
    cl::desc("The log level, default is info"),
};

}  // namespace

int main(int argc, const char** argv) {
    llvm::InitLLVM guard(argc, argv);
    llvm::setBugReportMsg(
        "Please report bugs to https://github.com/clice-io/clice/issues and include the crash backtrace");
    cl::SetVersionPrinter([](llvm::raw_ostream& os) {
        os << std::format("clice version: {}\nllvm version: {}\n",
                          clice::config::version,
                          clice::config::llvm_version);
    });
    cl::HideUnrelatedOptions(category);
    cl::ParseCommandLineOptions(argc,
                                argv,
                                "clice is a new generation of language server for C/C++");

    logging::options.color = log_color;
    logging::options.level = log_level;
    logging::stderr_logger("clice", logging::options);

    if(auto result = fs::init_resource_dir(argv[0]); !result) {
        LOG_FATAL("Cannot find default resource directory, because {}", result.error());
    }

    for(int i = 0; i < argc; ++i) {
        LOG_INFO("argv[{}] = {}", i, argv[i]);
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
            LOG_INFO("Server starts listening on stdin/stdout");
            break;
        }

        case Mode::Socket: {
            async::net::listen(host.c_str(), port, loop);
            LOG_INFO("Server starts listening on {}:{}", host.getValue(), port.getValue());
            break;
        }

        case Mode::Indexer: {
            /// TODO:
            break;
        }
    }

    async::run();

    LOG_INFO("clice exit normally!");

    return 0;
}
