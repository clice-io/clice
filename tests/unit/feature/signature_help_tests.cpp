#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

namespace protocol = eventide::language::protocol;

TEST_SUITE(SignatureHelp) {

Tester tester;
protocol::SignatureHelp help;

void run(llvm::StringRef code) {
    tester.clear();
    tester.add_main("main.cpp", code);
    tester.prepare();

    tester.params.completion = {"main.cpp", tester.nameless_points()[0]};

    help = feature::signature_help(tester.params, {});
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
