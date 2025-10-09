#include <thread>
#include "Test/Tester.h"
#include "Compiler/Compilation.h"
#include "Support/FileSystem.h"

namespace clice::testing {

namespace {

suite<"Compiler"> compiler = [] {
    test("TopLevelDecls") = [] {
        Tester tester;

        llvm::StringRef content = R"(
#include <iostream>

int x = 1;

void foo {}

namespace foo2 {
    int y = 2;
    int z = 3;
}

struct Bar {
    int x;
    int y;
};
)";

        tester.add_main("main.cpp", content);
        fatal / expect(tester.compile_with_pch());
        expect(that % tester.unit->top_level_decls().size() == 4);
    };

    test("StopCompilation") = [] {
        std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);

        Tester tester;
        tester.params.stop = stop;

        llvm::StringRef content = R"(
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <optional>
)";
        tester.add_main("main.cpp", content);

        bool result = true;

        std::thread thread([&]() { result = tester.compile_with_pch(); });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stop->store(true);

        thread.join();

        expect(that % !result);
    };
};

}  // namespace

}  // namespace clice::testing
