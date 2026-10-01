/// The code an action writes must compile where it lands and mean what the
/// source meant: each case applies an action to the buffer and recompiles
/// it, and the fixtures' static_asserts fail when a spelled name finds a
/// different entity there.

import type * as proto from "vscode-languageserver-protocol";
import type { CliceClient } from "@clice/tools/client";
import { applyTextEdits, editsFor } from "@clice/tools/client/edits";
import { parseAnnotations } from "@clice/tools/snap/annotation";
import { expect, test, type SessionFactory } from "../fixtures.ts";

function positionOf(text: string, offset: number): proto.Position {
    const before = text.slice(0, offset);
    const line = before.split("\n").length - 1;
    return { line, character: offset - (before.lastIndexOf("\n") + 1) };
}

/// The opened main.cpp of `source` (annotated with `§(name)` points),
/// compiled as C++23 and formatted in LLVM style.
async function open(session: SessionFactory, source: string) {
    const annotated = parseAnnotations(source);
    const workspace = session.tmpdir();
    workspace.write(".clang-format", "BasedOnStyle: LLVM\n");
    workspace.write("main.cpp", annotated.content);
    workspace.writeCDB(["main.cpp"], { std: "c++23" });
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");
    return new OpenFile(client, uri, annotated.content, annotated.offsets);
}

const expanded = expect.stringMatching(/^Replace /);

class OpenFile {
    private readonly client: CliceClient;
    private readonly uri: string;
    private readonly original: string;
    private readonly markers: Map<string, number>;
    private version = 0;

    constructor(client: CliceClient, uri: string, original: string, markers: Map<string, number>) {
        this.client = client;
        this.uri = uri;
        this.original = original;
        this.markers = markers;
    }

    async titles(marker: string): Promise<string[]> {
        return (await this.actions(marker)).map((action) => action.title);
    }

    /// Apply the action titled `title` at `marker` to the original text:
    /// the result must compile cleanly. The buffer returns to the original
    /// text afterwards.
    async apply(marker: string, title: string): Promise<string> {
        const actions = await this.actions(marker);
        const action = actions.find((candidate) => candidate.title === title);
        expect(
            action,
            `${title} among ${JSON.stringify(actions.map((a) => a.title))}`,
        ).toBeDefined();
        const edited = applyTextEdits(this.original, editsFor(action!, this.uri));
        await this.change(edited);
        this.client.assertCleanCompile(this.uri);
        await this.change(this.original);
        return edited;
    }

    private async actions(marker: string): Promise<proto.CodeAction[]> {
        const offset = this.markers.get(marker);
        expect(offset, marker).toBeDefined();
        const position = positionOf(this.original, offset!);
        const reply = await this.client.codeActions(this.uri, { start: position, end: position });
        return (reply ?? []).filter((item): item is proto.CodeAction => "title" in item);
    }

    private async change(text: string): Promise<void> {
        this.version += 1;
        this.client.change(this.uri, this.version, text);
        await this.client.waitForRecompile(this.uri);
    }
}

test("define members of templates over auto parameters", async ({ session }) => {
    const buffer = await open(
        session,
        `template <class T>
concept Small = sizeof(T) <= 4;

template <auto V>
struct S {
    void §(plain)f();
};

template <Small auto V>
struct C {
    void §(constrained)g();
};

template <auto... Vs>
struct P {
    void §(pack)h();
};

template <auto V>
struct D {
    void a();
    void b();
};

template <auto V>
void D<V>::a() {
    §(body)int x = 0;
}
`,
    );
    expect(await buffer.apply("plain", "Define 'S<V>::f' out of line")).toContain(
        "template <auto V> void S<V>::f() {}",
    );
    expect(await buffer.apply("constrained", "Define 'C<V>::g' out of line")).toContain(
        "template <Small auto V> void C<V>::g() {}",
    );
    expect(await buffer.apply("pack", "Define 'P<Vs...>::h' out of line")).toContain(
        "template <auto... Vs> void P<Vs...>::h() {}",
    );
    expect(await buffer.apply("body", "Define missing members of 'D'")).toContain(
        "template <auto V> void D<V>::b() {}",
    );
});

test("expand auto through a decltype declared type", async ({ session }) => {
    const buffer = await open(
        session,
        `namespace n {
struct M {};
decltype(M{}) m;
decltype(M{})* pm;
}  // namespace n

void f() {
    §(qualified)auto x = n::m;
    static_assert(__is_same(decltype(x), n::M));
    §(pointer)auto y = n::pm;
    static_assert(__is_same(decltype(y), n::M*));
}

using namespace n;

void g() {
    §(directive)auto z = m;
    static_assert(__is_same(decltype(z), n::M));
}
`,
    );
    expect(await buffer.apply("qualified", "Replace 'auto' with 'n::M'")).toContain("n::M x");
    expect(await buffer.apply("pointer", "Replace 'auto' with 'decltype(n::M{})*'")).toContain(
        "decltype(n::M{})* y",
    );
    expect(await buffer.apply("directive", "Replace 'auto' with 'n::M'")).toContain("n::M z");
});

test("names keep the qualifiers shadowing needs", async ({ session }) => {
    const buffer = await open(
        session,
        `namespace a {
template <class T>
struct Box {};
struct X {};
X make();
Box<X> make_box();
namespace b {
struct X {};
void f() {
    §(inner)auto x = a::make();
    static_assert(__is_same(decltype(x), a::X));
    §(argument)auto box = a::make_box();
    static_assert(__is_same(decltype(box), a::Box<a::X>));
}
}  // namespace b
}  // namespace a

struct G {};
G make_global();

namespace ns {
struct G {};
struct T {};
T make_t();
void f() {
    §(global)auto g = ::make_global();
    static_assert(__is_same(decltype(g), ::G));
}
template <class T>
void g() {
    §(parameter)auto t = make_t();
    static_assert(__is_same(decltype(t), ns::T));
}
}  // namespace ns
`,
    );
    expect(await buffer.apply("inner", "Replace 'auto' with 'a::X'")).toContain("a::X x");
    expect(await buffer.apply("argument", "Replace 'auto' with 'Box<a::X>'")).toContain(
        "Box<a::X> box",
    );
    expect(await buffer.apply("global", "Replace 'auto' with '::G'")).toContain("::G g");
    expect(await buffer.apply("parameter", "Replace 'auto' with 'ns::T'")).toContain("ns::T t");
});

test("internal type names become standard ones", async ({ session }) => {
    const buffer = await open(
        session,
        `void f() {
    §(size)auto n = sizeof(int);
    §(null)auto p = nullptr;
    static_assert(__is_same(decltype(p), decltype(nullptr)));
}

namespace std {
using size_t = decltype(sizeof 0);
using ptrdiff_t = decltype((int*)0 - (int*)0);
using nullptr_t = decltype(nullptr);
}  // namespace std

void g(int* a, int* b) {
    §(size_std)auto n = sizeof(int);
    §(difference)auto d = b - a;
    §(null_std)auto p = nullptr;
}
`,
    );
    expect(await buffer.titles("size")).not.toContainEqual(expanded);
    expect(await buffer.apply("null", "Replace 'auto' with 'decltype(nullptr)'")).toContain(
        "decltype(nullptr) p",
    );
    expect(await buffer.apply("size_std", "Replace 'auto' with 'std::size_t'")).toContain(
        "std::size_t n",
    );
    expect(await buffer.apply("difference", "Replace 'auto' with 'std::ptrdiff_t'")).toContain(
        "std::ptrdiff_t d",
    );
    expect(await buffer.apply("null_std", "Replace 'auto' with 'std::nullptr_t'")).toContain(
        "std::nullptr_t p",
    );
});

test("types the insertion point cannot name stay auto", async ({ session }) => {
    const buffer = await open(
        session,
        `auto local() {
    struct Hidden {};
    return Hidden{};
}

class C {
    struct Private {};

public:
    static Private make();

    void member() {
        §(member)auto p = make();
    }
};

void f() {
    struct Own {};
    §(own)auto o = Own{};
    §(local)auto h = local();
    §(private)auto p = C::make();
}
`,
    );
    expect(await buffer.titles("local")).not.toContainEqual(expanded);
    expect(await buffer.titles("private")).not.toContainEqual(expanded);
    expect(await buffer.apply("member", "Replace 'auto' with 'C::Private'")).toContain(
        "C::Private p",
    );
    expect(await buffer.apply("own", "Replace 'auto' with 'Own'")).toContain("Own o");
});

test("cv-qualifiers stay on the deduced pointer", async ({ session }) => {
    const buffer = await open(
        session,
        `void f(int* q, int** pp) {
    const §(pointer)auto p = q;
    static_assert(__is_same(decltype(p), int* const));
    const §(pointee)auto* cp = pp;
    static_assert(__is_same(decltype(cp), int* const*));
    static const §(specifiers)auto s = q;
    const static §(parted)auto t = q;
}
`,
    );
    expect(await buffer.apply("pointer", "Replace 'const auto' with 'int* const'")).toContain(
        "int* const p = q;",
    );
    expect(await buffer.apply("pointee", "Replace 'const auto' with 'int* const'")).toContain(
        "int* const* cp = pp;",
    );
    expect(await buffer.apply("specifiers", "Replace 'const auto' with 'int* const'")).toContain(
        "static int* const s = q;",
    );
    expect(await buffer.titles("parted")).not.toContainEqual(expanded);
});

test("one override per signature shared by bases", async ({ session }) => {
    const buffer = await open(
        session,
        `struct A {
    virtual void f() = 0;
    virtual void g() noexcept = 0;
};

struct B {
    virtual void f() = 0;
    virtual void g() = 0;
};

struct §(derived)D : A, B {};

static_assert(!__is_abstract(D));
`,
    );
    const edited = await buffer.apply("derived", "Implement pure virtual methods of 'D'");
    expect(edited).toContain("  void f() override;\n  void g() noexcept override;\n");
});

test("overrides repeat the specifiers they must", async ({ session }) => {
    const buffer = await open(
        session,
        `#define NOEXCEPT noexcept

namespace base {
constexpr bool flag = true;

struct Interface {
    virtual void log(...) = 0;
    virtual void macro() NOEXCEPT = 0;
    virtual void value() noexcept(flag) = 0;
    virtual void lax() noexcept(false) = 0;
    virtual consteval int compute() = 0;
};
}  // namespace base

struct §(derived)Impl : base::Interface {};

static_assert(!__is_abstract(Impl));

template <bool B>
struct Pending {
    virtual void wait() noexcept(B) = 0;
};

struct §(pending)Waiter : Pending<true> {};
`,
    );
    const edited = await buffer.apply("derived", "Implement pure virtual methods of 'Impl'");
    expect(edited).toContain(
        [
            "  void log(...) override;",
            "  void macro() noexcept override;",
            "  void value() noexcept override;",
            "  void lax() override;",
            "  consteval int compute() override;",
        ].join("\n"),
    );
    // The base's specification waits on instantiation: no spelling of it
    // is known to hold.
    expect(await buffer.titles("pending")).not.toContain(
        "Implement pure virtual methods of 'Waiter'",
    );
});
