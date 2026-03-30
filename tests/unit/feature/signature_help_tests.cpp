#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

namespace protocol = eventide::ipc::protocol;

TEST_SUITE(SignatureHelp, Tester) {

protocol::SignatureHelp help;

void run(llvm::StringRef code) {
    add_main("main.cpp", code);
    prepare();

    auto main_path = TestVFS::path("main.cpp");
    params.completion = {main_path, nameless_points()[0]};
    params.add_remapped_file(main_path, sources.all_files["main.cpp"].content);

    help = feature::signature_help(params, {});
}

TEST_CASE(Simple) {
    run(R"cpp(
void foo();

void foo(int x);

void foo(int x, int y);

int main() {
    foo($);
}
)cpp");

    ASSERT_EQ(help.signatures.size(), 3U);
}

};  // TEST_SUITE(SignatureHelp)

}  // namespace

}  // namespace clice::testing
