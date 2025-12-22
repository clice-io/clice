#include "Test/Test.h"
#include "Test/Tester.h"
#include "Feature/SignatureHelp.h"

namespace clice::testing {
namespace {
TEST_SUITE(SignatureHelp) {

Tester tester;
proto::SignatureHelp help;

void run(llvm::StringRef code) {
    tester.clear();
    tester.add_main("main.cpp", code);
    tester.prepare();

    tester.params.completion = {"main.cpp", tester.nameless_points()[0]};

    help = feature::signature_help(tester.params, {});
};

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

/// FIXME: Add more tests.

};  // TEST_SUITE(SignatureHelp)
}  // namespace
}  // namespace clice::testing
