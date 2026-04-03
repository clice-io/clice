#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "test/tester.h"
#include "semantic/find_target.h"
#include "semantic/selection.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/Decl.h"

namespace clice::testing {
namespace {

// todo: not all tests are adapted yet.
// Adapted from clangd's find-target suites:
// - TargetDeclTests:
//   https://github.com/llvm/llvm-project/blob/llvmorg-21.1.4/clang-tools-extra/clangd/unittests/FindTargetTests.cpp
// - AllRefsInFoo:
//   https://github.com/llvm/llvm-project/blob/llvmorg-21.1.4/clang-tools-extra/clangd/unittests/FindTargetTests.cpp
// - AllRefs:
//   https://github.com/llvm/llvm-project/blob/llvmorg-21.1.4/clang-tools-extra/clangd/unittests/FindTargetTests.cpp
//
// Covered here:
// - target-decl normalization for expressions, aliases, templates, using enum,
//   constructor/base initializers, and designated initializers
// - simple expressions/member expressions
// - namespace aliases and using declarations
// - qualified names and simple types
// - template specializations/aliases/template-template parameters
// - using-shadow references, macro references, broken-code recovery, and
//   implicit-node filtering
// - unresolved lookup, dependent scope references, declarations, ctor init,
//   using enum, namespace aliases, sizeof...(pack), CTAD, and designated
//   initializers
//
// Intentionally not ported yet:
// - Objective-C AllRefsInFoo / AllRefs coverage from clangd's upstream file;
//   `find_target.cpp` has ObjC visitors, but this local unit harness still
//   exercises only the C++ matrix that we can run reliably in the freestanding
//   test environment
// - unresolved/dependent edge cases where clangd intentionally snapshots empty
//   target sets for recovery-only spellings that the local resolver still
//   treats as implementation-defined
TEST_SUITE(FindExplicitReferences, Tester) {

struct AllRefs {
    std::string annotated_code;
    std::string dumped_references;
};

std::string dump_ref(ReferenceLoc ref) {
    std::string text;
    llvm::raw_string_ostream os(text);
    os << ref;
    return os.str();
}

std::string dump_decl(const clang::NamedDecl& decl) {
    std::string text;
    llvm::raw_string_ostream os(text);
    decl.print(os);

    llvm::StringRef printed = text;
    printed = printed.take_until([](char ch) { return ch == '{' || ch == ';'; });
    return printed.rtrim().str();
}

std::string dump_relations(DeclRelationSet relations) {
    std::string text;
    auto append = [&](llvm::StringRef name) {
        if(!text.empty()) {
            text += ", ";
        }
        text += name.str();
    };

    if(relations.contains(DeclRelation::Alias)) {
        append("Alias");
    }
    if(relations.contains(DeclRelation::Underlying)) {
        append("Underlying");
    }
    if(relations.contains(DeclRelation::TemplateInstantiation)) {
        append("TemplateInstantiation");
    }
    if(relations.contains(DeclRelation::TemplatePattern)) {
        append("TemplatePattern");
    }

    return text;
}

std::string dump_target_decl(TargetDecl target) {
    std::string text = dump_decl(*target.Decl);
    if(auto relations = dump_relations(target.Relations); !relations.empty()) {
        text += std::format(" [{}]", relations);
    }
    return text;
}

std::string dump_target_decls(llvm::SmallVector<TargetDecl, 1> decls) {
    std::vector<std::string> lines;
    lines.reserve(decls.size());
    for(const auto& decl: decls) {
        lines.push_back(dump_target_decl(decl));
    }

    std::sort(lines.begin(), lines.end());

    std::string dumped;
    for(const auto& line: lines) {
        dumped += line;
        dumped += '\n';
    }
    return dumped;
}

clang::Decl* find_top_level_decl(llvm::StringRef name) {
    for(auto* decl: unit->top_level_decls()) {
        if(auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
           named && named->getNameAsString() == name) {
            return decl;
        }
    }
    return nullptr;
}

AllRefs annotated_references(llvm::StringRef code, llvm::SmallVector<ReferenceLoc> refs) {
    auto& sm = unit->context().getSourceManager();
    llvm::stable_sort(refs, [&](const ReferenceLoc& lhs, const ReferenceLoc& rhs) {
        return sm.isBeforeInTranslationUnit(lhs.NameLoc, rhs.NameLoc);
    });

    std::string annotated_code;
    unsigned next_code_char = 0;
    for(unsigned i = 0; i < refs.size(); ++i) {
        auto pos = refs[i].NameLoc;
        if(!pos.isValid()) {
            return {};
        }
        if(pos.isMacroID()) {
            pos = sm.getExpansionLoc(pos);
        }
        if(!pos.isFileID()) {
            return {};
        }

        auto [file, offset] = sm.getDecomposedLoc(pos);
        if(file != sm.getMainFileID()) {
            continue;
        }

        if(!(next_code_char <= offset)) {
            return {};
        }
        annotated_code += code.substr(next_code_char, offset - next_code_char);
        annotated_code += std::format("$({})", i);
        next_code_char = offset;
    }
    annotated_code += code.substr(next_code_char);

    std::string dumped_references;
    for(unsigned i = 0; i < refs.size(); ++i) {
        dumped_references += std::format("{}: {}\n", i, dump_ref(refs[i]));
    }

    return {std::move(annotated_code), std::move(dumped_references)};
}

AllRefs annotate_references_in_foo(llvm::StringRef code, llvm::StringRef language = "c++") {
    clear();
    add_main("main.cpp", code);
    if(!compile("-std=c++20", language)) {
        return {};
    }

    auto* test_decl = find_top_level_decl("foo");
    if(!test_decl) {
        return {};
    }

    if(auto* templ = llvm::dyn_cast<clang::FunctionTemplateDecl>(test_decl)) {
        test_decl = templ->getTemplatedDecl();
    }

    llvm::SmallVector<ReferenceLoc> refs;
    if(const auto* func = llvm::dyn_cast<clang::FunctionDecl>(test_decl)) {
        explicit_references(
            func->getBody(),
            [&](ReferenceLoc ref) { refs.push_back(std::move(ref)); },
            &unit->resolver());
    } else if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(test_decl)) {
        explicit_references(
            ns,
            [&](ReferenceLoc ref) {
                if(ref.Targets.size() == 1 && ref.Targets.front() == ns) {
                    return;
                }
                refs.push_back(std::move(ref));
            },
            &unit->resolver());
    } else {
        return {};
    }

    return annotated_references(code, std::move(refs));
}

AllRefs annotate_references_in_file(llvm::StringRef code,
                                    llvm::StringRef language = "c++",
                                    llvm::StringRef standard = "-std=c++20") {
    clear();
    add_main("main.cpp", code);
    if(!compile(standard, language)) {
        return {};
    }

    llvm::SmallVector<ReferenceLoc> refs;
    explicit_references(
        unit->context(),
        [&](ReferenceLoc ref) { refs.push_back(std::move(ref)); },
        &unit->resolver());
    return annotated_references(code, std::move(refs));
}

std::string selected_target_decls(llvm::StringRef code,
                                  llvm::StringRef language = "c++",
                                  llvm::StringRef standard = "-std=c++20") {
    clear();
    add_main("main.cpp", code);
    if(!compile(standard, language)) {
        return "<compile failed>\n";
    }

    auto tree = SelectionTree::create_right(*unit, range());
    const auto* node = tree.common_ancestor();
    if(!node) {
        return "<no selection>\n";
    }

    return dump_target_decls(all_target_decls(node->data, &unit->resolver()));
}

void expect_target_decl_cases(
    std::initializer_list<std::pair<llvm::StringRef, llvm::StringRef>> cases,
    llvm::StringRef language = "c++",
    llvm::StringRef standard = "-std=c++20") {
    for(const auto& [annotated_code, expected_decls]: cases) {
        EXPECT_EQ(selected_target_decls(annotated_code, language, standard),
                  std::string(expected_decls));
    }
}

TEST_CASE(AllRefsInFoo) {
    std::pair<llvm::StringRef, llvm::StringRef> cases[] = {
        // Expressions.
        {R"cpp(
        int global;
        int func();
        void foo(int param) {
          $(0)global = $(1)param + $(2)func();
        }
        )cpp",
         "0: targets = {global}\n" "1: targets = {param}\n" "2: targets = {func}\n"                                                                                                                                                                                                                                                                                                                                                                                                            },
        {R"cpp(
        struct X { int a; };
        void foo(X x) {
          $(0)x.$(1)a = 10;
        }
        )cpp",
         "0: targets = {x}\n" "1: targets = {X::a}\n"                                                                                                                                                                                                                                                                                                                                                                                                                                          },

        // Broken code recovery.
        {R"cpp(
          // error-ok: testing with broken code
          int bar();
          int foo() {
            return $(0)bar() + $(1)bar(42);
          }
        )cpp",
         "0: targets = {bar}\n" "1: targets = {bar}\n"                                                                                                                                                                                                                                                                                                                                                                                                                                         },

        // Using directives, using declarations, and using enum.
        {R"cpp(
          namespace ns {}
          namespace alias = ns;
          void foo() {
            using namespace $(0)ns;
            using namespace $(1)alias;
          }
        )cpp",
         "0: targets = {ns}\n" "1: targets = {alias}\n"                                                                                                                                                                                                                                                                                                                                                                                                                                        },
        {R"cpp(
          namespace ns { int global; }
          void foo() {
            using $(0)ns::$(1)global;
          }
        )cpp",
         "0: targets = {ns}\n" "1: targets = {ns::global}, qualifier = 'ns::'\n"                                                                                                                                                                                                                                                                                                                                                                                                               },
        {R"cpp(
          namespace ns { enum class A {}; }
          void foo() {
            using enum $(0)ns::$(1)A;
          }
        )cpp",
         "0: targets = {ns}\n" "1: targets = {ns::A}, qualifier = 'ns::'\n"                                                                                                                                                                                                                                                                                                                                                                                                                    },

        // Qualified names and simple types.
        {R"cpp(
         struct Struct { int a; };
         using Typedef = int;
         void foo() {
           $(0)Struct $(1)x;
           $(2)Typedef $(3)y;
           static_cast<$(4)Struct*>(0);
         }
       )cpp",
         "0: targets = {Struct}\n" "1: targets = {x}, decl\n" "2: targets = {Typedef}\n" "3: targets = {y}, decl\n" "4: targets = {Struct}\n"                                                                                                                                                                                                                                                                                                                                                  },
        {R"cpp(
         namespace a { namespace b { struct S { typedef int type; }; } }
         void foo() {
           $(0)a::$(1)b::$(2)S $(3)x;
           using namespace $(4)a::$(5)b;
           $(6)S::$(7)type $(8)y;
         }
        )cpp",
         "0: targets = {a}\n" "1: targets = {a::b}, qualifier = 'a::'\n" "2: targets = {a::b::S}, qualifier = 'a::b::'\n" "3: targets = {x}, decl\n" "4: targets = {a}\n" "5: targets = {a::b}, qualifier = 'a::'\n" "6: targets = {a::b::S}\n" "7: targets = {a::b::S::type}, qualifier = 'S::'\n" "8: targets = {y}, decl\n"                                                                                                                                                                 },

        // Labels.
        {R"cpp(
         void foo() {
           $(0)ten:
           goto $(1)ten;
         }
       )cpp",
         "0: targets = {ten}, decl\n" "1: targets = {ten}\n"                                                                                                                                                                                                                                                                                                                                                                                                                                   },

        // Template specializations, aliases, and using-shadows.
        {R"cpp(
          template <class T> struct vector { using value_type = T; };
          template <> struct vector<bool> { using value_type = bool; };
          void foo() {
            $(0)vector<int> $(1)vi;
            $(2)vector<bool> $(3)vb;
          }
        )cpp",
         "0: targets = {vector<int>}\n" "1: targets = {vi}, decl\n" "2: targets = {vector<bool>}\n" "3: targets = {vb}, decl\n"                                                                                                                                                                                                                                                                                                                                                                },
        {R"cpp(
            template <class T> struct vector { using value_type = T; };
            template <> struct vector<bool> { using value_type = bool; };
            template <class T> using valias = vector<T>;
            void foo() {
              $(0)valias<int> $(1)vi;
              $(2)valias<bool> $(3)vb;
            }
          )cpp",
         "0: targets = {valias}\n" "1: targets = {vi}, decl\n" "2: targets = {valias}\n" "3: targets = {vb}, decl\n"                                                                                                                                                                                                                                                                                                                                                                           },
        {R"cpp(
            struct X { void func(int); };
            struct Y : X {
              using X::func;
            };
            void foo(Y y) {
              $(0)y.$(1)func(1);
            }
        )cpp",
         "0: targets = {y}\n" "1: targets = {Y::func}\n"                                                                                                                                                                                                                                                                                                                                                                                                                                       },
        {R"cpp(
            namespace ns { void bar(int); }
            using ns::bar;

            void foo() {
              $(0)bar(10);
            }
        )cpp",
         "0: targets = {bar}\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                },

        // Macros, range-for declarations, and unresolved lookup.
        {R"cpp(
            #define FOO a
            #define BAR b

            void foo(int a, int b) {
              $(0)FOO+$(1)BAR;
            }
        )cpp",
         "0: targets = {a}\n" "1: targets = {b}\n"                                                                                                                                                                                                                                                                                                                                                                                                                                             },
        {R"cpp(
            struct vector {
              int *begin();
              int *end();
            };

            void foo() {
              for (int $(0)x : $(1)vector()) {
                $(2)x = 10;
              }
            }
        )cpp",
         "0: targets = {x}, decl\n" "1: targets = {vector}\n" "2: targets = {x}\n"                                                                                                                                                                                                                                                                                                                                                                                                             },
        {R"cpp(
            namespace ns1 { void func(char*); }
            namespace ns2 { void func(int*); }
            using namespace ns1;
            using namespace ns2;

            template <class T>
            void foo(T t) {
              $(0)func($(1)t);
            }
        )cpp",
         "0: targets = {ns1::func, ns2::func}\n" "1: targets = {t}\n"                                                                                                                                                                                                                                                                                                                                                                                                                          },

        // Dependent scope references and template-template parameters.
        {R"cpp(
            template <class T>
            struct S {
              static int value;
            };

            template <class T>
            void foo() {
              $(0)S<$(1)T>::$(2)value;
            }
       )cpp",
         "0: targets = {S}\n" "1: targets = {T}\n" "2: targets = {S::value}, qualifier = 'S<T>::'\n"                                                                                                                                                                                                                                                                                                                                                                                           },
        {R"cpp(
            template <class T> struct vector {};

            template <template<class> class TT, template<class> class ...TP>
            void foo() {
              $(0)TT<int> $(1)x;
              $(2)foo<$(3)TT>();
              $(4)foo<$(5)vector>();
              $(6)foo<$(7)TP...>();
            }
        )cpp",
         "0: targets = {TT}\n" "1: targets = {x}, decl\n" "2: targets = {foo}\n" "3: targets = {TT}\n" "4: targets = {foo}\n" "5: targets = {vector}\n" "6: targets = {foo}\n" "7: targets = {TP}\n"                                                                                                                                                                                                                                                                                           },

        // Declarations and constructor initializers.
        {R"cpp(
             namespace ns {}
             class S {};
             void foo() {
               class $(0)Foo { $(1)Foo(); ~$(2)Foo(); int $(3)field; };
               int $(4)Var;
               enum $(5)E { $(6)ABC };
               typedef int $(7)INT;
               using $(8)INT2 = int;
               namespace $(9)NS = $(10)ns;
             }
           )cpp",
         "0: targets = {Foo}, decl\n" "1: targets = {foo()::Foo::Foo}, decl\n" "2: targets = {Foo}\n" "3: targets = {foo()::Foo::field}, decl\n" "4: targets = {Var}, decl\n" "5: targets = {E}, decl\n" "6: targets = {foo()::ABC}, decl\n" "7: targets = {INT}, decl\n" "8: targets = {INT2}, decl\n" "9: targets = {NS}, decl\n" "10: targets = {ns}\n"                                                                                                                                     },
        {R"cpp(
             class Base {};
             void foo() {
               class $(0)X {
                 int $(1)abc;
                 $(2)X(): $(3)abc() {}
               };
               class $(4)Derived : public $(5)Base {
                 $(6)Base $(7)B;
                 $(8)Derived() : $(9)Base() {}
               };
               class $(10)Foo {
                 $(11)Foo(int);
                 $(12)Foo(): $(13)Foo(111) {}
               };
             }
           )cpp",
         "0: targets = {X}, decl\n" "1: targets = {foo()::X::abc}, decl\n" "2: targets = {foo()::X::X}, decl\n" "3: targets = {foo()::X::abc}\n" "4: targets = {Derived}, decl\n" "5: targets = {Base}\n" "6: targets = {Base}\n" "7: targets = {foo()::Derived::B}, decl\n" "8: targets = {foo()::Derived::Derived}, decl\n" "9: targets = {Base}\n" "10: targets = {Foo}, decl\n" "11: targets = {foo()::Foo::Foo}, decl\n" "12: targets = {foo()::Foo::Foo}, decl\n" "13: targets = {Foo}\n"},

        // Namespace aliases.
        {R"cpp(
                namespace ns { struct Type {}; }
                namespace alias = ns;
                namespace rec_alias = alias;

                void foo() {
                  $(0)ns::$(1)Type $(2)a;
                  $(3)alias::$(4)Type $(5)b;
                  $(6)rec_alias::$(7)Type $(8)c;
                }
           )cpp",
         "0: targets = {ns}\n" "1: targets = {ns::Type}, qualifier = 'ns::'\n" "2: targets = {a}, decl\n" "3: targets = {alias}\n" "4: targets = {ns::Type}, qualifier = 'alias::'\n" "5: targets = {b}, decl\n" "6: targets = {rec_alias}\n" "7: targets = {ns::Type}, qualifier = 'rec_alias::'\n" "8: targets = {c}, decl\n"                                                                                                                                                                },

        // sizeof...(pack) and CTAD.
        {R"cpp(
                template <typename... E>
                void foo() {
                  constexpr int $(0)size = sizeof...($(1)E);
                };
            )cpp",
         "0: targets = {size}, decl\n" "1: targets = {E}\n"                                                                                                                                                                                                                                                                                                                                                                                                                                    },
        {R"cpp(
                template <typename T>
                struct Test {
                Test(T);
              };
                void foo() {
                  $(0)Test $(1)a(5);
                }
            )cpp",
         "0: targets = {Test}\n" "1: targets = {a}, decl\n"                                                                                                                                                                                                                                                                                                                                                                                                                                    },

        // Designated initializers.
        {R"cpp(
            void foo() {
              struct $(0)Foo {
                int $(1)Bar;
              };
              $(2)Foo $(3)f { .$(4)Bar = 42 };
            }
        )cpp",
         "0: targets = {Foo}, decl\n" "1: targets = {foo()::Foo::Bar}, decl\n" "2: targets = {Foo}\n" "3: targets = {f}, decl\n" "4: targets = {foo()::Foo::Bar}\n"                                                                                                                                                                                                                                                                                                                            },
        {R"cpp(
            void foo() {
              struct $(0)Baz {
                int $(1)Field;
              };
              struct $(2)Bar {
                $(3)Baz $(4)Foo;
              };
              $(5)Bar $(6)bar { .$(7)Foo.$(8)Field = 42 };
            }
        )cpp",
         "0: targets = {Baz}, decl\n" "1: targets = {foo()::Baz::Field}, decl\n" "2: targets = {Bar}, decl\n" "3: targets = {Baz}\n" "4: targets = {foo()::Bar::Foo}, decl\n" "5: targets = {Bar}\n" "6: targets = {bar}, decl\n" "7: targets = {foo()::Bar::Foo}\n" "8: targets = {foo()::Baz::Field}\n"                                                                                                                                                                                      },

        // Designated initializers in dependent code.
        {R"cpp(
            template <typename T>
            void crash(T) {}
            template <typename T>
            void foo() {
              $(0)crash({.$(1)x = $(2)T()});
            }
        )cpp",
         "0: targets = {crash}\n" "1: targets = {}\n" "2: targets = {T}\n"                                                                                                                                                                                                                                                                                                                                                                                                                     },
    };

    for(const auto& [expected_code, expected_refs]: cases) {
        auto actual = annotate_references_in_foo(expected_code);
        EXPECT_EQ(actual.dumped_references, std::string(expected_refs));
    }
}

TEST_CASE(AllRefs) {
    std::pair<llvm::StringRef, llvm::StringRef> cases[] = {
        // Unknown template name should not crash.
        {R"cpp(
            // error-ok: declarations use unknown template name
            template <typename T> struct Foo {
              using x = $(0)T::template $(1)A<0>;
            };
        )cpp",
         "0: targets = {Foo::T}, decl\n" "1: targets = {Foo}, decl\n" "2: targets = {Foo::x}, decl\n" "3: targets = {Foo::T}\n" "4: targets = {}, qualifier = 'T::'\n"                                                                               },

        // Deduction guides.
        {R"cpp(
            template<typename> struct $(0)A {};
            template<typename> struct $(1)I { using $(2)type = int; };
            template<typename $(3)T> A($(4)T) -> A<typename $(5)T::$(6)type>;
        )cpp",
         "0: targets = {A}, decl\n" "1: targets = {I}, decl\n" "2: targets = {I::type}, decl\n" "3: targets = {T}, decl\n" "4: targets = {A}\n" "5: targets = {T}\n" "6: targets = {A}\n" "7: targets = {T}\n" "8: targets = {}, qualifier = 'T::'\n"},
    };

    for(const auto& [annotated_code, expected_refs]: cases) {
        auto actual = annotate_references_in_file(annotated_code);
        EXPECT_EQ(actual.dumped_references, std::string(expected_refs));
    }
}

TEST_CASE(TargetDeclTests) {
    std::pair<llvm::StringRef, llvm::StringRef> cases[] = {
        // Expressions.
        {R"cpp(
            int f();
            int foo() { return @[f](); }
        )cpp",
         "int f()\n"                                                                            },
        {R"cpp(
            // error-ok: testing unresolved lookup recovery
            int f();
            int f(int, int);
            int foo(int x) { return @[f](x); }
        )cpp",
         "int f()\n" "int f(int, int)\n"                                                        },

        // Using declarations and using-shadows.
        {R"cpp(
            namespace foo { int f(int); }
            @[using foo::f];
        )cpp",
         "int f(int)\n" "using foo::f [Alias]\n"                                                },
        {R"cpp(
            struct X { int foo(); };
            struct Y : X { using X::foo; };
            int bar() { return Y().@[foo](); }
        )cpp",
         "int foo()\n" "using X::foo [Alias]\n"                                                 },

        // Namespace aliases and type aliases.
        {R"cpp(
            namespace ns { struct Type {}; }
            namespace alias = ns;
            void foo() { @[alias::]Type value; }
        )cpp",
         "namespace alias = ns [Alias]\n" "namespace ns [Underlying]\n"                         },
        {R"cpp(
            struct Foo {};
            using Alias = Foo;
            void foo() { @[Alias] value; }
        )cpp",
         "struct Foo [Underlying]\n" "using Alias = Foo [Alias]\n"                              },

        // Template specializations.
        {R"cpp(
            template <typename T>
            struct Box {};
            void foo() { @[Box<int>] value; }
        )cpp",
         "struct Box [TemplatePattern]\n" "template<> struct Box<int> [TemplateInstantiation]\n"},

        // Using enum.
        {R"cpp(
            namespace ns { enum class A {}; }
            using enum ns::@[A];
        )cpp",
         "enum class A : int\n"                                                                 },

        // Constructor initializers and base specifiers.
        {R"cpp(
            struct Base {};
            struct Derived : @[Base] {};
        )cpp",
         "struct Base\n"                                                                        },
        {R"cpp(
            struct S {
              int field;
              S() : @[field]() {}
            };
        )cpp",
         "int field\n"                                                                          },

        // Designated initializers.
        {R"cpp(
            void foo() {
              struct S { int bar; };
              S value{ @[.bar = 1] };
            }
        )cpp",
         "int bar\n"                                                                            },
    };

    for(const auto& [annotated_code, expected_decls]: cases) {
        EXPECT_EQ(selected_target_decls(annotated_code), std::string(expected_decls));
    }
}

TEST_CASE(Recovery) {
    expect_target_decl_cases({
        // Error recovery should still surface the viable overload set.
        {R"cpp(
            // error-ok: testing unresolved lookup recovery
            int f();
            int f(int, int);
            int foo(int x) { return @[f](x); }
        )cpp",
         "int f()\n" "int f(int, int)\n"},
    });
}

TEST_CASE(RecoveryType) {
    expect_target_decl_cases({
        // Recovering through an invalid call should still let us resolve the
        // selected member name from the produced object type.
        {R"cpp(
            // error-ok: keep going after the bad call
            struct S { int member; };
            S make(int);
            void foo() { make().@[member]; }
        )cpp",
         "int member\n"},
    });
}

TEST_CASE(DependentTypes) {
    expect_target_decl_cases({
        // Resolved to a dependent member in the primary template.
        {R"cpp(
            template <typename T>
            struct A {
              struct B {};
            };
            template <typename T>
            void foo() { typename A<T>::@[B] x; }
        )cpp",
         "struct B\n"                    },
        // Resolved to a nested type inside a dependent member.
        {R"cpp(
            template <typename T>
            struct A {
              struct B { struct C {}; };
            };
            template <typename T>
            void foo() { typename A<T>::@[B]::C x; }
        )cpp",
         "struct B\n"                    },
        // Dependent template names should preserve the written template.
        {R"cpp(
            template <typename T>
            struct A {
              template <typename>
              struct B {};
            };
            template <typename T>
            void foo() { typename A<T>::template @[B]<int> x; }
        )cpp",
         "template <typename> struct B\n"},
    });

    // Still intentionally unported from clangd's DependentTypes suite:
    // - selecting the nested dependent `C` in `A<T>::B::C`
    // - recursive alias cycles where clangd returns no targets
    // The local resolver currently diverges on those recovery-heavy cases.
}

TEST_CASE(TypedefCascade) {
    expect_target_decl_cases({
        // Alias chains should retain all written typedefs so callers can decide
        // whether to stop at the first alias or keep desugaring.
        {R"cpp(
            struct C { using type = int; };
            struct B { using type = C::type; };
            struct A { using type = B::type; };
            void foo() { A::@[type] value = 0; }
        )cpp",
         "using type = B::type [Alias]\n" "using type = C::type [Alias, Underlying]\n" "using type = int [Alias, Underlying]\n"},
    });
}

TEST_CASE(RecursiveTemplate) {
    expect_target_decl_cases({
        // The alias target should still be surfaced even when the recursive
        // branch keeps the underlying type dependent. The local resolver also
        // surfaces the constrained leaf target that feeds the specialization.
        {R"cpp(
            template <typename T> concept Leaf = false;
            template <typename Tree> struct descend_left {
              using type = typename descend_left<typename Tree::left>::type;
            };
            template <Leaf Tree> struct descend_left<Tree> {
              using type = Tree;
            };
            template <typename Tree>
            using left_most_leaf = typename descend_left<Tree>::@[type];
        )cpp",
         "Leaf Tree [Underlying]\n" "using type = Tree [Alias]\n"},
    });
}

TEST_CASE(DesignatedInit) {
    expect_target_decl_cases(
        {
            // C designators should resolve to the written field declaration.
            {R"c(
                struct Foo { int a; int b; };
                void foo(void) {
                  struct Foo value = { @[.a] = 1, .b = 2 };
                }
            )c",
             "int a\n"},
    },
        "c",
        "-std=c11");
}

};  // TEST_SUITE(FindExplicitReferences)
}  // namespace
}  // namespace clice::testing
