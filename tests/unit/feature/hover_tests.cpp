#include <optional>
#include <string>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

namespace protocol = kota::ipc::protocol;

TEST_SUITE(Hover, Tester) {

std::optional<protocol::Hover> result;

void run(llvm::StringRef code) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());

    auto points = nameless_points();
    ASSERT_EQ(points.size(), 1U);
    auto offset = points[0];
    result = feature::hover(*unit, offset, {}, feature::PositionEncoding::UTF8);
}

void run_with_options(llvm::StringRef code, feature::HoverOptions options) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());

    auto points = nameless_points();
    ASSERT_EQ(points.size(), 1U);
    auto offset = points[0];
    result = feature::hover(*unit, offset, options, feature::PositionEncoding::UTF8);
}

llvm::StringRef content() {
    return std::get<protocol::MarkupContent>(result->contents).value;
}

void expect_contains(llvm::StringRef expected) {
    ASSERT_TRUE(result.has_value());
    auto* mc = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(mc != nullptr);
    EXPECT_NE(mc->value.find(expected.str()), std::string::npos);
}

void expect_not_contains(llvm::StringRef unexpected) {
    ASSERT_TRUE(result.has_value());
    auto* mc = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(mc != nullptr);
    EXPECT_EQ(mc->value.find(unexpected.str()), std::string::npos);
}

// === Basic Declarations ===

TEST_CASE(Namespace) {
    run(R"cpp(
namespace $A {
}
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("namespace");
    expect_contains("`A`");
}

TEST_CASE(FunctionReference) {
    run(R"cpp(
int foo() { return 0; }
int x = $foo();
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("foo");
    expect_contains("int");
}

TEST_CASE(GlobalFunction) {
    run(R"cpp(
int compute(int a, double b) { return 0; }
int x = $compute(1, 2.0);
)cpp");

    expect_contains("function");
    expect_contains("`compute`");
    expect_contains("`int`");
    expect_contains("Parameters:");
    expect_contains("`int a`");
    expect_contains("`double b`");
}

TEST_CASE(MemberFunction) {
    run(R"cpp(
struct Foo {
    int $bar(int x) { return x; }
};
)cpp");

    expect_contains("method");
    expect_contains("`bar`");
    expect_contains("Parameters:");
    expect_contains("`int x`");
    expect_contains("// In Foo");
}

TEST_CASE(ConstructorDecl) {
    run(R"cpp(
struct Widget {
    Widget(int x, double y);
};
void use() { $Widget w(1, 2.0); }
)cpp");

    expect_contains("`Widget`");
    expect_contains("struct");
}

TEST_CASE(DestructorRef) {
    run(R"cpp(
struct Widget {
    ~Widget();
};
void use(Widget* p) { p->~$Widget(); }
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("Widget");
}

TEST_CASE(OperatorOverload) {
    run(R"cpp(
struct Vec {
    int val;
    Vec $operator+(const Vec& rhs) const { return {val + rhs.val}; }
};
)cpp");

    expect_contains("operator+");
    expect_contains("// In Vec");
}

TEST_CASE(GlobalVariable) {
    run(R"cpp(
int $global_var = 42;
)cpp");

    expect_contains("variable");
    expect_contains("`global_var`");
    expect_contains("Type:");
    expect_contains("`int`");
}

TEST_CASE(StaticMember) {
    run(R"cpp(
struct Foo {
    static int $count;
};
int Foo::count = 0;
)cpp");

    expect_contains("`count`");
    expect_contains("Type:");
    expect_contains("`int`");
    expect_contains("// In Foo");
}

TEST_CASE(ConstexprVariable) {
    run(R"cpp(
constexpr int $magic = 42;
)cpp");

    expect_contains("variable");
    expect_contains("`magic`");
    expect_contains("Value =");
    expect_contains("`42");
}

TEST_CASE(EnumMember) {
    run(R"cpp(
enum Color { Red = 1, $Green = 2, Blue = 3 };
)cpp");

    expect_contains("enum member");
    expect_contains("`Green`");
    expect_contains("Value =");
    expect_contains("`2`");
}

TEST_CASE(TypedefAlias) {
    run(R"cpp(
typedef unsigned long $ULong;
)cpp");

    expect_contains("type");
    expect_contains("`ULong`");
}

TEST_CASE(UsingAlias) {
    run(R"cpp(
using $Integer = int;
)cpp");

    expect_contains("type");
    expect_contains("`Integer`");
}

// === Scope Display ===

TEST_CASE(NamespaceScope) {
    run(R"cpp(
namespace foo {
namespace bar {
    int $val = 0;
}
}
)cpp");

    expect_contains("// In namespace foo::bar");
}

TEST_CASE(NestedClassScope) {
    run(R"cpp(
struct Outer {
    struct Inner {
        int $field;
    };
};
)cpp");

    expect_contains("// In Outer::Inner");
}

TEST_CASE(AnonymousNamespace) {
    run(R"cpp(
namespace {
    int $hidden = 0;
}
)cpp");

    expect_contains("variable");
    expect_contains("`hidden`");
}

TEST_CASE(FunctionLocalScope) {
    run(R"cpp(
void func() {
    int $local = 10;
}
)cpp");

    expect_contains("variable");
    expect_contains("`local`");
    expect_contains("// In func");
}

TEST_CASE(LambdaScope) {
    run(R"cpp(
void outer() {
    auto lam = [](int $x) { return x; };
}
)cpp");

    expect_contains("parameter");
    expect_contains("`x`");
}

// === Type Information ===

TEST_CASE(PointerType) {
    run(R"cpp(
int val = 0;
int* $ptr = &val;
)cpp");

    expect_contains("Type:");
    expect_contains("int *");
}

TEST_CASE(ReferenceType) {
    run(R"cpp(
int val = 0;
int& $ref = val;
)cpp");

    expect_contains("Type:");
    expect_contains("int &");
}

TEST_CASE(AutoDeduction) {
    run(R"cpp(
$auto x = 42;
)cpp");

    expect_contains("int");
}

TEST_CASE(DecltypeDeduction) {
    run(R"cpp(
int i = 0;
$decltype(i) j = 1;
)cpp");

    expect_contains("int");
}

TEST_CASE(ShowAka) {
    run(R"cpp(
typedef unsigned long MyType;
$MyType x = 0;
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("MyType");
}

TEST_CASE(DisableShowAka) {
    feature::HoverOptions opts;
    opts.show_aka = false;
    run_with_options(R"cpp(
typedef unsigned long MyType;
$MyType x = 0;
)cpp",
                     opts);

    ASSERT_TRUE(result.has_value());
    expect_not_contains("aka");
}

// === Function Signatures ===

TEST_CASE(FunctionReturnType) {
    run(R"cpp(
double $compute() { return 3.14; }
)cpp");

    expect_contains("`double`");
}

TEST_CASE(FunctionParameters) {
    run(R"cpp(
void $process(int count, const char* name, double ratio) {}
)cpp");

    expect_contains("Parameters:");
    expect_contains("`int count`");
    expect_contains("`const char * name`");
    expect_contains("`double ratio`");
}

TEST_CASE(DefaultArguments) {
    run(R"cpp(
void $init(int x = 10, int y = 20) {}
)cpp");

    expect_contains("Parameters:");
    expect_contains("10");
    expect_contains("20");
}

TEST_CASE(TemplateFunction) {
    run(R"cpp(
template<typename T>
T $identity(T val) { return val; }
)cpp");

    expect_contains("function");
    expect_contains("`identity`");
    expect_contains("template");
}

TEST_CASE(VariadicTemplate) {
    run(R"cpp(
template<typename... Args>
void $variadic(Args... args) {}
)cpp");

    expect_contains("function");
    expect_contains("`variadic`");
}

// === Template Parameters ===

TEST_CASE(ClassTemplate) {
    run(R"cpp(
template<typename T>
struct $Container { T value; };
)cpp");

    expect_contains("struct");
    expect_contains("`Container`");
    expect_contains("template");
}

TEST_CASE(TemplateWithDefaults) {
    run(R"cpp(
template<typename T, typename Alloc = void>
struct $MyVec {};
)cpp");

    expect_contains("struct");
    expect_contains("`MyVec`");
}

TEST_CASE(NonTypeTemplateParam) {
    run(R"cpp(
template<int N>
struct $FixedArray { int data[N]; };
)cpp");

    expect_contains("struct");
    expect_contains("`FixedArray`");
    expect_contains("template");
}

// === Documentation ===

TEST_CASE(BasicComment) {
    run(R"cpp(
/// This is documentation.
int $documented = 0;
)cpp");

    expect_contains("This is documentation.");
}

TEST_CASE(MultiLineComment) {
    run(R"cpp(
/// First line.
/// Second line.
void $multi() {}
)cpp");

    expect_contains("First line.");
    expect_contains("Second line.");
}

TEST_CASE(NoComment) {
    run(R"cpp(
int $no_doc = 0;
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("`no_doc`");
}

TEST_CASE(DoxygenParam) {
    run(R"cpp(
/// \param x The first value.
/// \param y The second value.
void $add(int x, int y) {}
)cpp");

    expect_contains("`x`");
    expect_contains("The first value.");
    expect_contains("`y`");
    expect_contains("The second value.");
    expect_not_contains("\\param");
}

TEST_CASE(DoxygenReturn) {
    run(R"cpp(
/// \return The sum of a and b.
int $sum(int a, int b) { return a + b; }
)cpp");

    expect_contains("**Returns:**");
    expect_contains("The sum of a and b.");
    expect_not_contains("\\return");
}

TEST_CASE(DoxygenBrief) {
    run(R"cpp(
/// \brief A brief description.
///
/// A detailed description.
void $func() {}
)cpp");

    expect_contains("A brief description.");
    expect_contains("A detailed description.");
    expect_not_contains("\\brief");
}

TEST_CASE(DoxygenInlineCode) {
    run(R"cpp(
/// Use \c nullptr for null values.
void $func() {}
)cpp");

    expect_contains("`nullptr`");
    expect_not_contains("\\c");
}

TEST_CASE(DoxygenNote) {
    run(R"cpp(
/// \note Thread safety is not guaranteed.
void $func() {}
)cpp");

    expect_contains("**Note:**");
    expect_contains("Thread safety is not guaranteed.");
    expect_not_contains("\\note");
}

TEST_CASE(DoxygenFull) {
    run(R"cpp(
/// \brief Compute the sum.
///
/// Adds two integers and returns the result.
///
/// \param a The left operand.
/// \param b The right operand.
/// \return The sum.
int $add(int a, int b) { return a + b; }
)cpp");

    expect_contains("Compute the sum.");
    expect_contains("Adds two integers");
    expect_contains("`a`");
    expect_contains("The left operand.");
    expect_contains("`b`");
    expect_contains("The right operand.");
    expect_contains("**Returns:**");
    expect_contains("The sum.");
    expect_not_contains("\\brief");
    expect_not_contains("\\param");
    expect_not_contains("\\return");
}

// === Layout Info ===

TEST_CASE(StructSize) {
    run(R"cpp(
struct $Point {
    double x;
    double y;
};
)cpp");

    expect_contains("Size:");
    expect_contains("alignment");
}

TEST_CASE(FieldOffset) {
    run(R"cpp(
struct Data {
    int a;
    int $b;
};
)cpp");

    expect_contains("field");
    expect_contains("`b`");
    expect_contains("Offset:");
    expect_contains("Size:");
}

TEST_CASE(FieldPadding) {
    run(R"cpp(
struct Padded {
    char $c;
    int i;
};
)cpp");

    expect_contains("Offset:");
    expect_contains("Size:");
    expect_contains("padding");
}

TEST_CASE(BitField) {
    run(R"cpp(
struct Flags {
    unsigned int $flag : 3;
};
)cpp");

    expect_contains("field");
    expect_contains("`flag`");
    expect_contains("Size:");
    expect_contains("bit");
}

TEST_CASE(UnionField) {
    run(R"cpp(
union Variant {
    int $i;
    float f;
};
)cpp");

    expect_contains("field");
    expect_contains("`i`");
    expect_contains("Size:");
    // Union fields should NOT have offset
    expect_not_contains("Offset:");
}

// === Macro Hover ===

TEST_CASE(ObjectMacro) {
    run(R"cpp(
#define MY_CONST 42
int x = $MY_CONST;
)cpp");

    expect_contains("macro");
    expect_contains("`MY_CONST`");
    expect_contains("#define MY_CONST 42");
}

TEST_CASE(FunctionMacro) {
    run(R"cpp(
#define ADD(a, b) ((a) + (b))
int x = $ADD(1, 2);
)cpp");

    expect_contains("macro");
    expect_contains("`ADD`");
    expect_contains("#define ADD");
}

TEST_CASE(MacroDefinition) {
    run(R"cpp(
#define $MARKER 100
)cpp");

    expect_contains("macro");
    expect_contains("`MARKER`");
    expect_contains("#define MARKER 100");
}

// === Expression Hover ===

TEST_CASE(ThisExpr) {
    run(R"cpp(
struct Obj {
    void method() { auto self = $this; }
};
)cpp");

    expect_contains("this");
    expect_contains("Obj");
}

TEST_CASE(ConstructorCall) {
    run(R"cpp(
struct Box {
    Box(int w, int h) {}
};
Box b = $Box(1, 2);
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("Box");
}

TEST_CASE(MemberAccess) {
    run(R"cpp(
struct Item { int value; };
void use() {
    Item it;
    int v = it.$value;
}
)cpp");

    expect_contains("field");
    expect_contains("`value`");
    expect_contains("Type:");
    expect_contains("`int`");
}

TEST_CASE(TypeLocHover) {
    run(R"cpp(
struct MyClass {};
$MyClass obj;
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("MyClass");
}

TEST_CASE(ExpressionValue) {
    run(R"cpp(
constexpr int a = 10;
constexpr int b = $a + 1;
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("Value =");
}

// === Edge Cases ===

TEST_CASE(ForwardDeclaration) {
    run(R"cpp(
struct $Forward;
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("`Forward`");
}

TEST_CASE(AnonymousStruct) {
    run(R"cpp(
struct Outer {
    struct { int $x; } inner;
};
)cpp");

    expect_contains("field");
    expect_contains("`x`");
}

TEST_CASE(NoHover) {
    add_main("main.cpp", R"cpp(
$
int x;
)cpp");
    ASSERT_TRUE(compile());

    auto points = nameless_points();
    ASSERT_EQ(points.size(), 1U);
    auto offset = points[0];
    result = feature::hover(*unit, offset, {}, feature::PositionEncoding::UTF8);

    EXPECT_FALSE(result.has_value());
}

TEST_CASE(ScopedEnum) {
    run(R"cpp(
enum class Direction { Up, Down, Left, Right };
Direction d = Direction::$Up;
)cpp");

    expect_contains("enum member");
    expect_contains("`Up`");
    expect_contains("Value =");
    expect_contains("`0`");
}

// === Access Specifiers ===

TEST_CASE(PublicMember) {
    run(R"cpp(
struct Foo {
public:
    int $pub;
};
)cpp");

    expect_contains("field");
    expect_contains("`pub`");
    // struct members are public by default, check definition block
    expect_contains("// In Foo");
}

TEST_CASE(PrivateMember) {
    run(R"cpp(
class Bar {
    int $priv;
};
)cpp");

    expect_contains("field");
    expect_contains("`priv`");
    expect_contains("private:");
}

// === Additional Tests ===

TEST_CASE(VoidFunction) {
    run(R"cpp(
void $noop() {}
)cpp");

    expect_contains("function");
    expect_contains("`noop`");
    expect_contains("`void`");
}

TEST_CASE(ConstMethod) {
    run(R"cpp(
struct Obj {
    int $get() const { return 0; }
};
)cpp");

    expect_contains("method");
    expect_contains("`get`");
    expect_contains("const");
}

TEST_CASE(StaticMethod) {
    run(R"cpp(
struct Factory {
    static Factory $create() { return {}; }
};
)cpp");

    expect_contains("method");
    expect_contains("`create`");
    expect_contains("static");
}

TEST_CASE(VirtualMethod) {
    run(R"cpp(
struct Base {
    virtual void $update() {}
};
)cpp");

    expect_contains("method");
    expect_contains("`update`");
    expect_contains("virtual");
}

TEST_CASE(PureVirtualMethod) {
    run(R"cpp(
struct Interface {
    virtual void $draw() = 0;
};
)cpp");

    expect_contains("method");
    expect_contains("`draw`");
    expect_contains("virtual");
    expect_contains("= 0");
}

TEST_CASE(EnumDefinition) {
    run(R"cpp(
enum Status { OK, Error };
void use($Status s) {}
)cpp");

    expect_contains("`Status`");
}

TEST_CASE(ScopedEnumDefinition) {
    run(R"cpp(
enum class Color : char { Red, Green, Blue };
void use($Color c) {}
)cpp");

    expect_contains("`Color`");
}

TEST_CASE(ClassDefinition) {
    run(R"cpp(
class $Widget {
    int x;
    int y;
};
)cpp");

    expect_contains("class");
    expect_contains("`Widget`");
    expect_contains("Size:");
    expect_contains("alignment");
}

TEST_CASE(UnionDefinition) {
    run(R"cpp(
union $Data {
    int i;
    float f;
};
)cpp");

    expect_contains("union");
    expect_contains("`Data`");
    expect_contains("Size:");
}

TEST_CASE(ParameterHover) {
    run(R"cpp(
void func(int $param) {}
)cpp");

    expect_contains("parameter");
    expect_contains("`param`");
    expect_contains("Type:");
    expect_contains("`int`");
}

TEST_CASE(ConstexprBigValue) {
    run(R"cpp(
constexpr int $big = 255;
)cpp");

    expect_contains("Value =");
    expect_contains("0xff");
}

TEST_CASE(NegativeEnumValue) {
    run(R"cpp(
enum Signed { $Neg = -5 };
)cpp");

    expect_contains("enum member");
    expect_contains("`Neg`");
    expect_contains("-5");
}

TEST_CASE(TemplateTypeParameter) {
    run(R"cpp(
template<typename $T>
struct Box { T val; };
)cpp");

    ASSERT_TRUE(result.has_value());
    expect_contains("`T`");
}

TEST_CASE(MultipleParameters) {
    run(R"cpp(
void $multi(int a, int b, int c, int d) {}
)cpp");

    expect_contains("Parameters:");
    expect_contains("`int a`");
    expect_contains("`int b`");
    expect_contains("`int c`");
    expect_contains("`int d`");
}

TEST_CASE(FunctionPointerVar) {
    run(R"cpp(
int add(int a, int b) { return a + b; }
auto $fp = &add;
)cpp");

    expect_contains("`fp`");
}

TEST_CASE(NestedNamespaceVar) {
    run(R"cpp(
namespace a { namespace b { namespace c {
    int $deep = 0;
}}}
)cpp");

    expect_contains("// In namespace a::b::c");
}

};  // TEST_SUITE(Hover)

}  // namespace

}  // namespace clice::testing
