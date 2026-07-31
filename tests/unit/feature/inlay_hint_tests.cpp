/// Primary inlay-hint coverage lives in the snapshot corpus
/// (tests/snap/inlay_hint/), which pins both the standalone and the server
/// path under default options. This file keeps only the categories the
/// corpus cannot reach: block-end and default-argument hints are off by
/// default and only selectable through InlayHintsOptions.

#include <format>
#include <string>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

#include "kota/meta/enum.h"

namespace clice::testing {

namespace {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

TEST_SUITE(inlay_hint, Tester) {

std::vector<protocol::InlayHint> hints;
llvm::DenseMap<std::uint32_t, protocol::InlayHint> hints_map;

void run(llvm::StringRef code,
         const feature::InlayHintsOptions& options = {},
         std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile_with_pch("-std=c++23"));

    LocalSourceRange range = LocalSourceRange(0, unit->interested_content().size());
    hints = feature::inlay_hints(*unit, range, options, feature::PositionEncoding::UTF8);

    hints_map.clear();
    auto content = unit->interested_content();
    auto line_starts = unit->line_starts();
    lsp::LineMap map(content, line_starts, feature::PositionEncoding::UTF8);
    for(auto& hint: hints) {
        hints_map[*map.to_offset(hint.position)] = hint;
    }

    if(!unit->diagnostics().empty()) {
        for(auto& diagnostic: unit->diagnostics()) {
            std::println("{}", diagnostic.message);
        }
    }

    ASSERT_TRUE(unit->diagnostics().empty());
}

void EXPECT_SIZE(std::uint32_t size,
                 std::source_location location = std::source_location::current()) {
    ASSERT_EQ(hints.size(), size);
}

void EXPECT_HINT(llvm::StringRef pos,
                 llvm::StringRef name,
                 std::source_location location = std::source_location::current()) {
    auto offset = point(pos);
    auto it = hints_map.find(offset);
    ASSERT_TRUE(it != hints_map.end());

    std::string label;
    if(auto* plain = std::get_if<std::string>(&it->second.label)) {
        label = *plain;
    } else {
        for(const auto& part:
            std::get<std::vector<protocol::InlayHintLabelPart>>(it->second.label)) {
            label += part.value;
        }
    }
    ASSERT_EQ(label, name);
};

TEST_CASE(BlockEnd) {
    // Functions
    run(R"c(
            int foo() {
                return 41;
            }§(0)

            template<int X>
            int bar() {
                // No hint for lambda for now
                auto f = []() {
                    return X;
                };
                return f();
            }§(1)

            // No hint because this isn't a definition
            int buz();

            struct S{};
            bool operator==(S, S) {
                return true;
            }§(2)
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(3);
    EXPECT_HINT("0", "// foo");
    EXPECT_HINT("1", "// bar");
    EXPECT_HINT("2", "// operator==");

    // Methods
    run(R"c(
            struct Test {
                // No hint because there's no function body
                Test() = default;

                ~Test() {
                }§(0)

                void method1() {
                }§(1)

                // No hint because this isn't a definition
                void method2();

                template <typename T>
                void method3() {
                }§(2)

                // No hint because this isn't a definition
                template <typename T>
                void method4();

                Test operator+(int) const {
                    return *this;
                }§(3)

                operator bool() const {
                    return true;
                }§(4)

                // No hint because there's no function body
                operator int() const = delete;
            } x;

            void Test::method2() {
            }§(5)

            template <typename T>
            void Test::method4() {
            }§(6)
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(7);
    EXPECT_HINT("0", "// ~Test");
    EXPECT_HINT("1", "// method1");
    EXPECT_HINT("2", "// method3");
    EXPECT_HINT("3", "// operator+");
    EXPECT_HINT("4", "// operator bool");
    EXPECT_HINT("5", "// Test::method2");
    EXPECT_HINT("6", "// Test::method4");

    // Namespaces
    run(R"c(
            namespace {
                void foo();
            }§(0)

            namespace ns {
                void bar();
            }§(1)
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "// namespace");
    EXPECT_HINT("1", "// namespace ns");

    // Types
    run(R"c(
            struct S {
            };§(0)

            class C {
            };§(1)

            union U {
            };§(2)

            enum E1 {
            };§(3)

            enum class E2 {
            };§(4)
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(5);
    EXPECT_HINT("0", "// struct S");
    EXPECT_HINT("1", "// class C");
    EXPECT_HINT("2", "// union U");
    EXPECT_HINT("3", "// enum E1");
    EXPECT_HINT("4", "// enum class E2");

    // If statements
    run(R"c(
            void foo(bool cond) {
                if (cond)
                    ;

                if (cond) {
                }§(0)

                if (cond) {
                } else {
                }§(1)

                if (cond) {
                } else if (!cond) {
                }§(2)

                if (cond) {
                } else {
                    if (!cond) {
                    }§(3)
                }§(4)

                if (auto X = cond) {
                }§(5)

                if (int i = 0; i > 10) {
                }§(6)
            }
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(8);
    EXPECT_HINT("0", "// if cond");
    EXPECT_HINT("1", "// if cond");
    EXPECT_HINT("2", "// if");
    EXPECT_HINT("3", "// if !cond");
    EXPECT_HINT("4", "// if cond");
    EXPECT_HINT("5", "// if X");
    EXPECT_HINT("6", "// if i > 10");

    // `if consteval` has no condition and hints as plain "// if".
    run(R"c(
            void foo() {
                if consteval {
                    int x = 1;
                }§(0)
            }§(1)
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "// if");
    EXPECT_HINT("1", "// foo");

    // Loops
    run(R"c(
            void foo() {
                while (true)
                    ;

                while (true) {
                }§(0)

                do {
                } while (true);

                for (;true;) {
                }§(1)

                for (int I = 0; I < 10; ++I) {
                }§(2)

                int Vs[] = {1,2,3};
                for (auto V : Vs) {
                }§(3)
            }
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(5);
    EXPECT_HINT("0", "// while true");
    EXPECT_HINT("1", "// for true");
    EXPECT_HINT("2", "// for I");
    EXPECT_HINT("3", "// for V");

    // Switch
    run(R"c(
            void foo(int I) {
                switch (I) {
                    case 0: break;
                }§(0)
            }
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "// switch I");

    // Print literals
    run(R"c(
            #pragma clang diagnostic ignored "-Wliteral-conversion"
            void foo() {
                while ("foo") {
                }§(0)

                while ("foo but this time it is very long") {
                }§(1)

                while (true) {
                }§(2)

                while (1) {
                }§(3)

                while (1.5) {
                }§(4)
            }
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(6);
    EXPECT_HINT("0", "// while \"foo\"");
    EXPECT_HINT("1", "// while \"foo but...\"");
    EXPECT_HINT("2", "// while true");
    EXPECT_HINT("3", "// while 1");
    EXPECT_HINT("4", "// while 1.5");

    // Print refs
    run(R"c(
            namespace ns {
                int Var;
                int func();
                struct S {
                    int Field;
                    int method() const;
                };
            }
            void foo() {
                while (ns::Var) {
                }§(0)

                while (ns::func()) {
                }§(1)

                while (ns::S{}.Field) {
                }§(2)

                while (ns::S{}.method()) {
                }§(3)
            }
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(7);
    EXPECT_HINT("0", "// while Var");
    EXPECT_HINT("1", "// while func");
    EXPECT_HINT("2", "// while Field");
    EXPECT_HINT("3", "// while method");

    // Print conversions
    run(R"c(
            struct S {
                S(int);
                S(int, int);
                explicit operator bool();
            };
            void foo(int I) {
                while (float(I)) {
                }§(0)

                while (S(I)) {
                }§(1)

                while (S(I, I)) {
                }§(2)
            }
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(5);
    EXPECT_HINT("0", "// while float");
    EXPECT_HINT("1", "// while S");
    EXPECT_HINT("2", "// while S");

    // Print operators
    run(R"c(
            using Integer = int;
            void foo(Integer I) {
                while(++I){
                }§(0)

                while(I++){
                }§(1)

                while(+(I + I)){
                }§(2)

                while(I < 0){
                }§(3)

                while((I + I) < I){
                }§(4)

                while(I < (I + I)){
                }§(5)

                while((I + I) < (I + I)){
                }§(6)
            }
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(8);
    EXPECT_HINT("0", "// while ++I");
    EXPECT_HINT("1", "// while I++");
    EXPECT_HINT("2", "// while");
    EXPECT_HINT("3", "// while I < 0");
    EXPECT_HINT("4", "// while ... < I");
    EXPECT_HINT("5", "// while I < ...");
    EXPECT_HINT("6", "// while");

    // Trailing semicolon
    run(R"c(
            // The hint is placed after the trailing ';'
            struct S1 {
            }  ;§(0)

            // The hint is always placed in the same line with the closing '}'.
            // So in this case where ';' is missing, it is attached to '}'.
            struct S2 {
            }§(1)

            ;

            // No hint because only one trailing ';' is allowed
            struct S3 {
            };;

            // No hint because trailing ';' is only allowed for class/struct/union/enum
            void foo() {
            };

            // Rare case, but yes we'll have a hint here.
            struct {
                int x;
            }§(2)

            s2;
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(3);
    EXPECT_HINT("0", "// struct S1");
    EXPECT_HINT("1", "// struct S2");
    EXPECT_HINT("2", "// struct");

    // Trailing text
    run(R"c(
            struct S1 {
            }      ;§(0)

            // No hint for S2 because of the trailing comment
            struct S2 {
            }; /* Put anything here */

            struct S3 {
                // No hint for S4 because of the trailing source code
                struct S4 {
                };};§(1)

            // No hint for ns because of the trailing comment
            namespace ns {
            } // namespace ns
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "// struct S1");
    EXPECT_HINT("1", "// struct S3");

    // Macro
    run(R"c(
            #define DECL_STRUCT(NAME) struct NAME {
            #define RBRACE }

            DECL_STRUCT(S1)
            };§(0)

            // No hint because we require a '}'
            DECL_STRUCT(S2)
            RBRACE;
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "// struct S1");

    // Pointer to member function
    run(R"c(
            class A {};
            using Predicate = bool(A::*)();
            void foo(A* a, Predicate p) {
                if ((a->*p)()) {
                }§(0)
            }
        )c",
        {.parameters = false, .deduced_types = false, .designators = false, .block_end = true});
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "// if");
};

TEST_CASE(DefaultArguments) {
    // Smoke test
    run(R"c(
            int foo(int A = 4) { return A; }
            int bar(int A, int B = 1, bool C = foo(§(0))) { return A; }
            int A = bar(§(1)2§(2));

            void baz(int = 5) { if (false) baz(§(3)); };
        )c",
        {.default_arguments = true});
    EXPECT_SIZE(4);
    EXPECT_HINT("0", "A: 4");
    EXPECT_HINT("1", "A:");
    EXPECT_HINT("2", ", B: 1, C: foo()");
    EXPECT_HINT("3", "5");

    // Packs and constructor calls
    run(R"c(
            struct Baz {
                Baz(float a = 3 //
                            + 2);
            };
            struct Foo {
                Foo(int, Baz baz = //
                        Baz{§(0)}

                    //
                ) {}
            };

            void use() {
                Foo foo1(1§(1));
                Foo foo2{2§(2)};
                Foo foo3 = {3§(3)};
                auto foo4 = Foo{4§(4)};
            }
        )c",
        {.default_arguments = true});
    EXPECT_SIZE(6);
    EXPECT_HINT("0", "a: ...");
    EXPECT_HINT("1", ", baz: Baz{}");
    EXPECT_HINT("2", ", baz: Baz{}");
    EXPECT_HINT("3", ", baz: Baz{}");
    EXPECT_HINT("4", ", baz: Baz{}");

    // type_name_limit = 0 means unlimited: a default longer than the
    // usual limit still prints in full.
    run(R"c(
            void configure(int first, const char* name = "this default is longer than the limit");
            void use() { configure(1§(0)); }
        )c",
        {.default_arguments = true, .type_name_limit = 0});
    EXPECT_SIZE(2);
    EXPECT_HINT("0", R"(, name: "this default is longer than the limit")");

    // Default-argument hints survive with parameter hints disabled; the
    // parameter name drops out with them.
    run(R"c(
            void log(int level, bool flush = true);
            void use() { log(2§(0)); }
        )c",
        {.parameters = false, .default_arguments = true});
    EXPECT_SIZE(1);
    EXPECT_HINT("0", ", true");
};

TEST_CASE(EnabledOff) {
    run(R"c(
            void draw(int width, int height);
            void use() {
                auto value = 1;
                draw(value, 2);
            }
        )c",
        {.enabled = false});
    EXPECT_SIZE(0);
};

TEST_CASE(FreestandingBuiltins) {
    // The tester compiles with -ffreestanding, which strips library-builtin
    // IDs: std::forward suppression must hold through the name fallback.
    // The snap corpus compiles hosted and only reaches the builtin-ID path.
    run(R"c(
            namespace std { template <typename T> T&& forward(T&); }
            void use() {
                int i = 0;
                // Without the fallback this argument would carry a stray
                // `&` reference hint.
                int&& s = std::forward(i);
            }
        )c");
    EXPECT_SIZE(0);
};

};  // TEST_SUITE(inlay_hint)

}  // namespace
}  // namespace clice::testing
