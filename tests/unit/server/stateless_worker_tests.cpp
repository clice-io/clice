#include <string>
#include <vector>

#include "test/test.h"
#include "server/worker_test_helpers.h"
#include "worker/protocol.h"

#include "kota/codec/bincode/bincode.h"

namespace clice::testing {

namespace {

// ============================================================================
// Bincode Serialization Tests
// ============================================================================

ZEST_SUITE(BincodeRoundTrip) {

ZEST_CASE(CompileParamsRoundTrip) {
    namespace bincode = kota::codec::bincode;

    worker::CompileParams params;
    params.path = "/tmp/test.cpp";
    params.version = 1;
    params.text = "int main() { return 0; }";
    params.directory = "/tmp";
    params.arguments = {"clang++", "-c", "test.cpp"};
    params.pch = {"", 0};
    params.pcms = {};

    auto bytes = bincode::to_bytes(params);
    ZASSERT(bytes);

    worker::CompileParams result;
    auto status =
        bincode::from_bytes(std::span<const std::byte>(bytes->data(), bytes->size()), result);
    ZASSERT(status);

    ZEXPECT(result.path == params.path);
    ZEXPECT(result.version == params.version);
    ZEXPECT(result.text == params.text);
    ZEXPECT(result.directory == params.directory);
    ZEXPECT(result.arguments.size() == params.arguments.size());
}

ZEST_CASE(CompileResultRoundTrip) {
    namespace bincode = kota::codec::bincode;

    worker::CompileResult result;
    result.version = 1;
    result.diagnostics = {};  // empty

    auto bytes = bincode::to_bytes(result);
    ZASSERT(bytes);

    worker::CompileResult decoded;
    auto status =
        bincode::from_bytes(std::span<const std::byte>(bytes->data(), bytes->size()), decoded);
    ZASSERT(status);
    ZEXPECT(decoded.version == result.version);
}

};  // ZEST_SUITE(BincodeRoundTrip)

// ============================================================================
// StatelessWorker Tests
// ============================================================================

ZEST_SUITE(StatelessWorker) {

ZEST_CASE(SpawnAndExit) {
    WorkerHandle w;
    ZASSERT(w.spawn());

    w.run([]() -> kota::task<> { co_return; });
}

ZEST_CASE(BuildPCHRequest) {
    TempDir tmp;
    tmp.touch("test_pch.h", "#pragma once\nint pch_global = 42;\n");
    auto hdr = tmp.path("test_pch.h");

    WorkerHandle w;
    ZASSERT(w.spawn());

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::BuildPCHParams params;
        params.file = hdr;
        params.directory = "/tmp";
        params.arguments =
            {"clang++", "-resource-dir", std::string(resource_dir()), "-x", "c++-header", hdr};
        params.content = "#pragma once\nint pch_global = 42;\n";
        params.output_path = tmp.path("test_pch.pch");
        // The pair is mandatory: a PCH is only served with its blob.
        params.index_output_path = tmp.path("test_pch.pch.idx");

        auto result = co_await w.peer->send_request(params);
        ZEXPECT(result);
        if(!result.has_value()) {
            w.peer->close_output();
            co_return;
        }
        ZEXPECT(result.value().success);
        ZEXPECT(!result.value().output_path.empty());
        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

ZEST_CASE(IndexRequest) {
    TempDir tmp;
    tmp.touch("test_index.cpp", "int indexed_var = 1;\n");
    auto src = tmp.path("test_index.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn());

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::TURunParams params;
        params.index = true;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        auto result = co_await w.peer->send_request(params);
        ZEXPECT(result);
        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

};  // ZEST_SUITE(StatelessWorker)

// ============================================================================
// StatelessWorker Extended Tests
// ============================================================================

ZEST_SUITE(StatelessWorkerExtended) {

ZEST_CASE(BuildPCMRequest) {
    TempDir tmp;
    tmp.touch("test_module.cppm",
              "export module test_module;\nexport int module_func() { return 1; }\n");
    auto src = tmp.path("test_module.cppm");

    WorkerHandle w;
    ZASSERT(w.spawn());

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::BuildPCMParams params;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-std=c++20",
                            "--precompile",
                            src};
        params.module_name = "test_module";
        params.output_path = tmp.path("test_module.pcm");

        auto result = co_await w.peer->send_request(params);
        ZEXPECT(result);
        if(result.has_value()) {
            auto& build = result.value();
            ZEXPECT(build.success);
            ZEXPECT(build.build_at > 0);
            // The module source itself must be a hashed dependency: the PCM
            // cache key embeds no content, so the deps snapshot is the only
            // thing that can see an offline edit of the interface. Deps name
            // files by identity (on macOS the temp dir sits behind the
            // /var -> /private/var symlink).
            auto identity = CanonicalPath(Spelling::absolute(src)).str();
            bool source_dep = false;
            for(auto& dep: build.deps) {
                if(dep.path == identity) {
                    source_dep = dep.hash != 0;
                }
            }
            ZEXPECT(source_dep);
        }
        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

ZEST_CASE(CompletionRequest) {
    std::string text = "int foo = 1;\nint bar = fo";
    TempDir tmp;
    tmp.touch("completion_test.cpp", text);
    auto src = tmp.path("completion_test.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn());

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::CompletionParams params;
        params.file = src;
        params.text = text;
        params.directory = "/tmp";
        params.arguments = make_args(src);
        params.offset = 25;  // after "fo" in "int bar = fo" (13 + 12)

        auto result = co_await w.peer->send_request(params);
        ZEXPECT(result);
        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

ZEST_CASE(SignatureHelpRequest) {
    std::string text = "void foo(int a, int b) {}\nint main() { foo(";
    TempDir tmp;
    tmp.touch("sighelp_test.cpp", text);
    auto src = tmp.path("sighelp_test.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn());

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::SignatureHelpParams params;
        params.file = src;
        params.text = text;
        params.directory = "/tmp";
        params.arguments = make_args(src);
        params.offset = 45;  // after "foo(" (26 + 19)

        auto result = co_await w.peer->send_request(params);
        ZEXPECT(result);
        // Should return signature help for foo(int a, int b).
        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

// #701: ordering two constructor templates of a class template whose pack
// follows another parameter crashed clang (xclang patches/0001).
ZEST_CASE(ConstructorTemplatesOfClassTemplate) {
    std::string text = R"(template <int> struct index {};
template <class, class...> struct V {
  template <int I> V(index<I>);
  template <class = void> V(index<0>);
  void f() { V(); }
};
)";
    TempDir tmp;
    tmp.touch("ctor_templates.cpp", text);
    auto src = tmp.path("ctor_templates.cpp");
    auto offset = static_cast<uint32_t>(text.find("V();") + 2);

    WorkerHandle w;
    ZASSERT(w.spawn());

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::CompletionParams completion;
        completion.file = src;
        completion.text = text;
        completion.directory = "/tmp";
        completion.arguments = make_args(src);
        completion.offset = offset;
        auto completed = co_await w.peer->send_request(completion);
        ZEXPECT(completed);

        worker::SignatureHelpParams help;
        help.file = src;
        help.text = text;
        help.directory = "/tmp";
        help.arguments = make_args(src);
        help.offset = offset;
        auto helped = co_await w.peer->send_request(help);
        ZEXPECT(helped);

        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

ZEST_CASE(MultipleStatelessRequests) {
    TempDir tmp;
    std::vector<std::string> paths;
    for(int i = 0; i < 3; i++) {
        auto name = "multi_index_" + std::to_string(i) + ".cpp";
        auto text = "int idx_var_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
        tmp.touch(name, text);
        paths.push_back(tmp.path(name));
    }

    WorkerHandle w;
    ZASSERT(w.spawn());

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        // Send multiple index requests to test stateless worker handles them sequentially.
        for(int i = 0; i < 3; i++) {
            worker::TURunParams params;
            params.index = true;
            params.file = paths[i];
            params.directory = "/tmp";
            params.arguments = make_args(paths[i]);

            auto result = co_await w.peer->send_request(params);
            ZEXPECT(result);
        }
        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

};  // ZEST_SUITE(StatelessWorkerExtended)

}  // namespace

}  // namespace clice::testing
