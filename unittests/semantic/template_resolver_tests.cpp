module clice;

import :semantic.template_resolver;
import :semantic.semantics;
import :support.logging;
import :unittests.utils.tester;

namespace clice::testing {

namespace {

struct InputFinder : clang::RecursiveASTVisitor<InputFinder> {
    CompilationUnit& unit;
    clang::QualType input;

    using Base = clang::RecursiveASTVisitor<InputFinder>;

    InputFinder(CompilationUnit& unit) : unit(unit) {}

    bool TraverseDecl(clang::Decl* decl) {
        if(decl && (llvm::isa<clang::TranslationUnitDecl>(decl) ||
                    unit.file_id(decl->getLocation()) == unit.focus_file().file())) {
            Base::TraverseDecl(decl);
        }

        return true;
    }

    bool VisitTypedefNameDecl(const clang::TypedefNameDecl* decl) {
        if(decl->getName() == "input") {
            input = decl->getUnderlyingType();
        }

        return true;
    }
};

/// Aborts the test, naming the query and the program, when one query of the
/// resolver outlives `limit`: an unbounded loop or an exponential walk hangs
/// the server the same way it would hang here.
class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds limit) : limit(limit), thread([this] { watch(); }) {}

    Watchdog(const Watchdog&) = delete;

    ~Watchdog() {
        stopping = true;
        thread.join();
    }

    /// Run `query`, `kind` naming it, under the watch.
    void run(llvm::StringRef kind, llvm::function_ref<void()> query) {
        {
            std::lock_guard lock(mutex);
            current = kind.str();
            started = chrono::steady_clock::now();
            busy = true;
        }
        query();
        auto took = chrono::steady_clock::now() - started;
        std::lock_guard lock(mutex);
        busy = false;
        if(took > slowest) {
            slowest = took;
            slowest_kind = current;
        }
    }

    /// The program the queries are about, shown when one hangs.
    void about(std::string text) {
        std::lock_guard lock(mutex);
        program = std::move(text);
    }

    /// The slowest query so far and what it was.
    std::pair<chrono::milliseconds, std::string> slowest_query() {
        std::lock_guard lock(mutex);
        return {chrono::duration_cast<chrono::milliseconds>(slowest), slowest_kind};
    }

private:
    void watch() {
        while(!stopping) {
            // Hang detection: a query that never returns signals nothing;
            // only polling the clock tells it from a slow one.
            this_thread::sleep_for(chrono::milliseconds(100));
            std::lock_guard lock(mutex);
            if(busy && chrono::steady_clock::now() - started > limit) {
                logging::critical("template resolver query '{}' ran past {}s on:\n{}",
                                  current,
                                  limit.count(),
                                  program);
            }
        }
    }

    std::chrono::seconds limit;

    std::mutex mutex;

    std::string current;

    std::string program;

    chrono::steady_clock::time_point started;

    bool busy = false;

    chrono::steady_clock::duration slowest{};

    std::string slowest_kind;

    std::atomic<bool> stopping = false;

    std::thread thread;
};

/// Every query a feature makes of the resolver, made on every node of the
/// unit it applies to, with the preconditions the features keep. Results
/// are irrelevant: under the Debug build's ASan and assertions, finishing is
/// the assertion.
struct Scanner : clang::RecursiveASTVisitor<Scanner> {
    types::TemplateResolver& resolver;

    Watchdog& watchdog;

    unsigned queries = 0;

    void query(llvm::StringRef kind, llvm::function_ref<void()> run) {
        watchdog.run(kind, run);
        queries += 1;
    }

    /// What a declaration's dependent type resolves to, the class it names
    /// and what `->` reaches from it, as hover, completion and type
    /// display ask.
    void resolve(clang::QualType type) {
        if(type.isNull() || !type->isDependentType()) {
            return;
        }
        query("resolve", [&] { resolver.resolve(type); });
        query("resolve_tag", [&] { resolver.resolve_tag(type); });
        query("specializable", [&] { resolver.specializable(type); });
        query("arrow_pointee", [&] { resolver.arrow_pointee(type); });
    }

    bool VisitTypedefNameDecl(clang::TypedefNameDecl* decl) {
        resolve(decl->getUnderlyingType());
        return true;
    }

    bool VisitDeclaratorDecl(clang::DeclaratorDecl* decl) {
        resolve(decl->getType());
        return true;
    }

    bool VisitCXXRecordDecl(clang::CXXRecordDecl* decl) {
        if(decl->hasDefinition()) {
            for(auto& base: decl->bases()) {
                resolve(base.getType());
            }
        }
        return true;
    }

    bool VisitDependentNameTypeLoc(clang::DependentNameTypeLoc loc) {
        query("lookup dependent name", [&] { resolver.lookup(loc.getTypePtr()); });
        return true;
    }

    bool VisitTemplateSpecializationTypeLoc(clang::TemplateSpecializationTypeLoc loc) {
        query("lookup dependent template", [&] { resolver.lookup(loc.getTypePtr()); });
        return true;
    }

    bool VisitCXXDependentScopeMemberExpr(clang::CXXDependentScopeMemberExpr* expr) {
        query("lookup member", [&] { resolver.lookup(expr); });
        if(!expr->isImplicitAccess()) {
            query("member_object",
                  [&] { resolver.member_object(expr->getBase(), expr->isArrow()); });
        }
        return true;
    }

    bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* expr) {
        query("lookup qualified", [&] { resolver.lookup(expr); });
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* call) {
        if(!call->isTypeDependent()) {
            return true;
        }
        query("callees", [&] { resolver.callees(call); });
        llvm::ArrayRef<const clang::Expr*> arguments(call->getArgs(), call->getNumArgs());
        for(std::size_t written = 0; written <= arguments.size(); written += 1) {
            query("written callees",
                  [&] { resolver.callees(call->getCallee(), arguments.take_front(written)); });
        }
        return true;
    }

    bool VisitVarDecl(clang::VarDecl* var) {
        if(var->getType()->getContainedAutoType()) {
            query("deduce", [&] { resolver.deduce(var); });
        }
        return true;
    }

    bool VisitUnresolvedUsingValueDecl(clang::UnresolvedUsingValueDecl* decl) {
        query("lookup using", [&] { resolver.lookup(decl); });
        return true;
    }

    bool VisitUnresolvedUsingTypenameDecl(clang::UnresolvedUsingTypenameDecl* decl) {
        query("lookup using typename", [&] { resolver.lookup(decl); });
        return true;
    }

    bool VisitClassTemplateSpecializationDecl(clang::ClassTemplateSpecializationDecl* decl) {
        if(!llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(decl)) {
            query("instantiation_pattern", [&] { types::instantiation_pattern(*decl); });
        }
        return true;
    }

    bool VisitVarTemplateSpecializationDecl(clang::VarTemplateSpecializationDecl* decl) {
        if(!llvm::isa<clang::VarTemplatePartialSpecializationDecl>(decl)) {
            query("instantiation_pattern", [&] { types::instantiation_pattern(*decl); });
        }
        return true;
    }

    /// Scan the whole unit, then resolve the occurrences of every node of
    /// the focus file's semantic map, as indexing and hover do.
    void scan(CompilationUnit& unit) {
        TraverseAST(unit.context());
        auto semantics = Semantics::build(unit);
        for(std::uint32_t index = 0; index < semantics.node_entries().size(); index += 1) {
            query("resolve_occurrences", [&] { resolve_occurrences(semantics, index, &resolver); });
        }
    }
};

/// Random programs of dependent code, the shapes the resolver's guards
/// exist for: templates naming one another in cycles, bases that recurse
/// into themselves, partial and explicit specializations, values that do not
/// fold, long call chains, and source cut and spliced into broken code. One
/// seed gives one program on every platform.
class Generator {
public:
    explicit Generator(std::uint64_t seed) : random(seed) {}

    std::string program() {
        std::string text = prelude.str();
        for(unsigned index = 0, count = 1 + pick(6); index < count; index += 1) {
            text += class_template(index);
        }
        for(unsigned index = 0, count = 1 + pick(3); index < count; index += 1) {
            text += function_template(index);
        }
        if(chance(2)) {
            for(unsigned cuts = 1 + pick(3); cuts > 0; cuts -= 1) {
                corrupt(text);
            }
        }
        return text;
    }

private:
    /// What a template's body may name: its type parameters, its value
    /// parameters, its packs.
    struct Scope {
        llvm::SmallVector<std::string, 4> types;

        llvm::SmallVector<std::string, 2> values;

        llvm::SmallVector<std::string, 2> packs;

        llvm::SmallVector<std::string, 2> variables;
    };

    unsigned pick(unsigned bound) {
        return static_cast<unsigned>(random() % bound);
    }

    bool chance(unsigned in) {
        return pick(in) == 0;
    }

    template <typename T>
    const T& any(llvm::ArrayRef<T> choices) {
        return choices[pick(static_cast<unsigned>(choices.size()))];
    }

    /// Arguments for template `index`, by its shape; a defaulted one left
    /// to its default. The first type is one of the scope's parameters:
    /// clang instantiates a generated template's concrete specialization
    /// at once, and those recurse into one another until clang itself runs
    /// out of memory or stack.
    std::string arguments(unsigned index, const Scope& scope, unsigned depth) {
        llvm::SmallVector<std::string, 4> written;
        bool dependent = false;
        for(char shape: shapes[index]) {
            switch(shape) {
                case 't': {
                    auto first = any<std::string>(scope.types);
                    /// Named in its own definition with its own parameters, a
                    /// template is the current instantiation, whose members
                    /// clang expands at once; one that grows instead
                    /// (`t0<box<T>>`) recurses only through the resolver.
                    if(defining && index + 1 == shapes.size()) {
                        first = "fz::box<" + first + ">";
                    }
                    written.push_back(dependent ? type(scope, depth + 1) : first);
                    dependent = true;
                    break;
                }
                case 'n':
                    written.push_back(chance(3) ? std::string("0") : value(scope, depth + 1));
                    break;
                case 'b': written.push_back(value(scope, depth + 1)); break;
                case 'p': {
                    written.push_back(scope.packs.empty() ? type(scope, depth + 1)
                                                          : scope.packs.front() + "...");
                    break;
                }
                default: break;
            }
        }
        return llvm::join(written, ", ");
    }

    /// A specialization of a template declared so far, the one being
    /// declared included: it may name itself.
    std::string specialization(const Scope& scope, unsigned depth) {
        if(shapes.empty() || scope.types.empty() || chance(6)) {
            return "fz::box<" + type(scope, depth + 1) + ">";
        }
        auto target = pick(static_cast<unsigned>(shapes.size()));
        if(!llvm::StringRef(shapes[target]).contains('t')) {
            return "fz::box<" + type(scope, depth + 1) + ">";
        }
        return std::format("fz::t{}<{}>", target, arguments(target, scope, depth));
    }

    std::string type(const Scope& scope, unsigned depth) {
        if(depth > 3) {
            return scope.types.empty() ? "int" : any<std::string>(scope.types);
        }
        constexpr static llvm::StringLiteral members[] = {"type",
                                                          "next",
                                                          "nested::type",
                                                          "missing",
                                                          "value_type"};
        switch(pick(16)) {
            case 0:
            case 1: return scope.types.empty() ? "int" : any<std::string>(scope.types);
            case 2: return any<llvm::StringLiteral>({"int", "void", "char", "long"}).str();
            case 3: return "fz::box<" + type(scope, depth + 1) + ">";
            case 4:
                return "fz::pair<" + type(scope, depth + 1) + ", " + type(scope, depth + 1) + ">";
            case 5:
            case 6:
                return "typename " + specialization(scope, depth) +
                       "::" + any<llvm::StringLiteral>(members).str();
            case 7:
                return "typename " + specialization(scope, depth) + "::template inner<" +
                       type(scope, depth + 1) + ">::type";
            case 8:
                return "fz::conditional_t<" + value(scope, depth + 1) + ", " +
                       type(scope, depth + 1) + ", " + type(scope, depth + 1) + ">";
            case 9:
                return "typename fz::enable_if<" + value(scope, depth + 1) + ", " +
                       type(scope, depth + 1) + ">::type";
            case 10: return "decltype(" + expression(scope, depth + 1) + ")";
            case 11:
                return type(scope, depth + 1) +
                       any<llvm::StringLiteral>({"*", "&", "&&", " const"}).str();
            case 12: return "typename fz::remove_ref<" + type(scope, depth + 1) + ">::type";
            case 13:
                return scope.packs.empty() ? specialization(scope, depth)
                                           : "fz::list<" + scope.packs.front() + "...>";
            case 14:
                return "typename " + specialization(scope, depth) + "::template alias<" +
                       type(scope, depth + 1) + ">";
            default: return specialization(scope, depth);
        }
    }

    std::string value(const Scope& scope, unsigned depth) {
        if(depth > 4) {
            return scope.values.empty() ? "true" : any<std::string>(scope.values);
        }
        switch(pick(11)) {
            case 0: return any<llvm::StringLiteral>({"true", "false", "1", "0"}).str();
            case 1: return "(sizeof(" + type(scope, depth + 1) + ") > 4)";
            case 2:
                return "fz::is_same_v<" + type(scope, depth + 1) + ", " + type(scope, depth + 1) +
                       ">";
            case 3:
                return "fz::is_same<" + type(scope, depth + 1) + ", " + type(scope, depth + 1) +
                       ">::value";
            case 4:
                return "__is_same(" + type(scope, depth + 1) + ", " + type(scope, depth + 1) + ")";
            case 5: return specialization(scope, depth) + "::value";
            case 6:
                return scope.values.empty() ? "false"
                                            : "(" + any<std::string>(scope.values) + " + 1 > 2)";
            case 7: return "(fz::count<" + std::to_string(pick(4)) + ">::value > 1)";
            case 8: return "(" + value(scope, depth + 1) + " && " + value(scope, depth + 1) + ")";
            case 9: return "!" + value(scope, depth + 1);
            default: return scope.values.empty() ? "true" : any<std::string>(scope.values);
        }
    }

    std::string expression(const Scope& scope, unsigned depth) {
        if(depth > 5) {
            return scope.variables.empty() ? "0" : any<std::string>(scope.variables);
        }
        switch(pick(13)) {
            case 0:
            case 1: return scope.variables.empty() ? "0" : any<std::string>(scope.variables);
            case 2: return "fz::declval<" + type(scope, depth + 1) + ">()";
            case 3: return specialization(scope, depth) + "{}";
            case 4: return expression(scope, depth + 1) + ".get()";
            case 5: return expression(scope, depth + 1) + "->item";
            case 6: return expression(scope, depth + 1) + ".self().self()";
            case 7: return expression(scope, depth + 1) + "[0]";
            case 8: return "*" + expression(scope, depth + 1);
            case 9:
                return expression(scope, depth + 1) + ".template make<" + type(scope, depth + 1) +
                       ">(" + expression(scope, depth + 1) + ")";
            case 10: return "fz::call(" + expression(scope, depth + 1) + ")";
            case 11:
                return "call(" + expression(scope, depth + 1) + ", " +
                       expression(scope, depth + 1) + ")";
            default:
                return expression(scope, depth + 1) + ".f(" + expression(scope, depth + 1) + ")";
        }
    }

    /// The parameters of a template, with what its body may name.
    std::string parameters(llvm::StringRef shape, llvm::StringRef prefix, Scope& scope) {
        std::string text;
        for(auto [index, kind]: views::enumerate(shape)) {
            if(!text.empty()) {
                text += ", ";
            }
            auto name = std::format("{}{}", prefix, index);
            switch(kind) {
                case 't': {
                    text += "typename " + name;
                    scope.types.push_back(name);
                    break;
                }
                case 'n': {
                    text += "int " + name;
                    scope.values.push_back(name);
                    break;
                }
                case 'b': {
                    text += "bool " + name;
                    scope.values.push_back(name);
                    break;
                }
                default: {
                    text += "typename... " + name;
                    scope.packs.push_back(name);
                }
            }
        }
        return text;
    }

    std::string members(unsigned index, const Scope& scope) {
        std::string text;
        Scope inner = scope;
        inner.types.push_back("U");
        inner.variables.push_back("u");
        std::bitset<12> declared;
        for(unsigned count = 2 + pick(6); count > 0; count -= 1) {
            auto member = pick(12);
            /// Once each: a second `type` is a redefinition, which clang
            /// drops, and the generator wants the first one read.
            if(declared.test(member)) {
                continue;
            }
            declared.set(member);
            switch(member) {
                case 0: text += "    using type = " + type(scope, 0) + ";\n"; break;
                case 1: text += "    using next = " + type(scope, 0) + ";\n"; break;
                case 2:
                    text += "    static constexpr bool value = " + value(scope, 0) + ";\n";
                    break;
                case 3:
                    text +=
                        "    template <typename U> struct inner { using type = " + type(inner, 0) +
                        "; };\n";
                    break;
                case 4:
                    text += "    template <typename U> using alias = " + type(inner, 0) + ";\n";
                    break;
                case 5:
                    text += "    struct nested { using type = " + type(scope, 0) + "; };\n";
                    break;
                case 6: text += "    " + type(scope, 0) + " item;\n"; break;
                case 7:
                    text += "    auto get() -> " + type(scope, 0) + ";\n    t" +
                            std::to_string(index) + "& self();\n";
                    break;
                case 8:
                    text += "    template <typename U> auto make(U u) -> decltype(" +
                            expression(inner, 0) + ");\n";
                    break;
                case 9:
                    text += "    void f(int);\n    auto f(" + type(scope, 0) + ") const -> " +
                            type(scope, 0) + ";\n    auto f(const " + type(scope, 0) +
                            "&&) && -> int;\n";
                    break;
                case 10:
                    text += "    auto operator*() -> " + type(scope, 0) +
                            ";\n    auto operator->() -> " + type(scope, 0) +
                            "*;\n    auto operator[](int) -> " + type(scope, 0) +
                            ";\n    auto begin() -> " + type(scope, 0) + "*;\n    auto end() -> " +
                            type(scope, 0) + "*;\n";
                    break;
                default:
                    text += "    friend auto call(const t" + std::to_string(index) + "&, int) -> " +
                            type(scope, 0) + ";\n";
            }
        }
        return text;
    }

    std::string class_template(unsigned index) {
        defining = true;
        auto done = llvm::make_scope_exit([&] { defining = false; });
        constexpr static llvm::StringLiteral shapes_by_kind[] =
            {"t", "tt", "tn", "tb", "tp", "b", "ttb"};
        auto shape = any<llvm::StringLiteral>(shapes_by_kind).str();
        shapes.push_back(shape);
        Scope scope;
        auto prefix = std::format("P{}_", index);
        std::string text = "namespace fz {\ntemplate <" + parameters(shape, prefix, scope);
        /// A defaulted trailing parameter, which a detector's partial
        /// specialization fills with a `void_t` probe.
        bool detector = chance(4);
        if(detector) {
            text += ", typename = void";
            shapes.back() += 'd';
        }
        text += std::format(">\nstruct t{}", index);
        llvm::SmallVector<std::string, 2> bases;
        if(!scope.types.empty() && chance(3)) {
            bases.push_back(scope.types.front());
        }
        if(chance(3)) {
            bases.push_back(specialization(scope, 2));
        }
        if(!bases.empty()) {
            text += " : " + llvm::join(bases, ", ");
        }
        text += " {\n";
        for(auto& base: bases) {
            if(chance(2)) {
                text += "    using " + base +
                        "::" + any<llvm::StringLiteral>({"type", "get", "f", "value"}).str() +
                        ";\n";
            }
        }
        text += members(index, scope) + "};\n";

        auto argument = [&](char kind, unsigned position, Scope& partial) -> std::string {
            auto name = std::format("Q{}_{}", index, position);
            switch(kind) {
                case 't': {
                    /// A parameter named twice (`t<T, T>`).
                    if(!partial.types.empty() && chance(5)) {
                        return partial.types.front();
                    }
                    partial.types.push_back(name);
                    switch(pick(4)) {
                        case 0: return name + "*";
                        case 1: return "const " + name + "&";
                        case 2: return "fz::box<" + name + ">";
                        default: return name;
                    }
                }
                case 'n': return std::to_string(pick(3));
                case 'b': return chance(2) ? "false" : "true";
                case 'd':
                    return "fz::void_t<typename " +
                           (partial.types.empty() ? std::string("int") : partial.types.front()) +
                           "::type>";
                default: {
                    partial.packs.push_back(name);
                    return name + "...";
                }
            }
        };
        if(chance(2)) {
            Scope partial;
            llvm::SmallVector<std::string> written;
            for(auto [position, kind]: views::enumerate(shapes.back())) {
                written.push_back(argument(kind, static_cast<unsigned>(position), partial));
            }
            llvm::SmallVector<std::string> declared;
            for(auto& name: partial.types) {
                if(!ranges::contains(declared, "typename " + name)) {
                    declared.push_back("typename " + name);
                }
            }
            for(auto& name: partial.packs) {
                declared.push_back("typename... " + name);
            }
            if(!declared.empty()) {
                text += "template <" + llvm::join(declared, ", ") + ">\nstruct t" +
                        std::to_string(index) + "<" + llvm::join(written, ", ") + "> {\n" +
                        members(index, partial) + "};\n";
            }
        }
        if(chance(4)) {
            llvm::SmallVector<std::string> concrete;
            for(char kind: shapes.back()) {
                concrete.push_back(kind == 'n'   ? "0"
                                   : kind == 'b' ? "false"
                                   : kind == 'd' ? "void"
                                                 : "int");
            }
            text += "template <>\nstruct t" + std::to_string(index) + "<" +
                    llvm::join(concrete, ", ") + ">";
            text += chance(2) ? std::string(";\n") : " {\n" + members(index, Scope{}) + "};\n";
        }
        return text + "}  // namespace fz\n";
    }

    std::string function_template(unsigned index) {
        Scope scope{
            .types = {"X", "Y"},
            .variables = {"x", "y"}
        };
        std::string text = std::format(
            "namespace fz {{\ntemplate <typename X, typename Y>\nvoid use{}(X x, Y y) {{\n",
            index);
        for(unsigned count = 2 + pick(8), local = 0; count > 0; count -= 1) {
            switch(pick(7)) {
                case 0:
                    text += std::format("    auto a{} = {};\n", local++, expression(scope, 0));
                    break;
                case 1:
                    text += std::format("    decltype(auto) b{} = ({});\n",
                                        local++,
                                        expression(scope, 0));
                    break;
                case 2: text += std::format("    {} c{}{{}};\n", type(scope, 0), local++); break;
                case 3:
                    text += "    for(auto&& e: " + expression(scope, 0) + ") { e.get(); }\n";
                    break;
                case 4:
                    text += std::format("    using input{} = {};\n", local++, type(scope, 0));
                    break;
                case 5:
                    text +=
                        "    auto lambda = [](auto v) { return v.get().self(); };\n    lambda(" +
                        expression(scope, 0) + ");\n";
                    break;
                default: text += "    " + expression(scope, 0) + ";\n";
            }
        }
        return text + "}\n}  // namespace fz\n";
    }

    /// Cut, splice or duplicate a piece of `text`, as an edit in progress
    /// leaves it.
    void corrupt(std::string& text) {
        auto at = pick(static_cast<unsigned>(text.size()));
        switch(pick(4)) {
            case 0: text.erase(at, 1 + pick(12)); break;
            case 1:
                text.insert(at,
                            any<llvm::StringLiteral>({"<",
                                                      ">",
                                                      "::",
                                                      "typename ",
                                                      "(",
                                                      ")",
                                                      "{",
                                                      "}",
                                                      ";",
                                                      "template ",
                                                      ".",
                                                      "->",
                                                      "..."})
                                .str());
                break;
            case 2:
                text.insert(at, text.substr(pick(static_cast<unsigned>(text.size())), pick(40)));
                break;
            default: text.resize(at);
        }
    }

    /// What every program declares first: the library its templates use.
    constexpr static llvm::StringLiteral prelude = R"code(
namespace fz {
template <bool B, typename T, typename F> struct conditional { using type = T; };
template <typename T, typename F> struct conditional<false, T, F> { using type = F; };
template <bool B, typename T, typename F> using conditional_t = typename conditional<B, T, F>::type;
template <bool B, typename T = void> struct enable_if {};
template <typename T> struct enable_if<true, T> { using type = T; };
template <typename T, typename U> struct is_same { static constexpr bool value = false; };
template <typename T> struct is_same<T, T> { static constexpr bool value = true; };
template <typename T, typename U> constexpr bool is_same_v = false;
template <typename T> constexpr bool is_same_v<T, T> = true;
template <typename...> using void_t = void;
template <typename T> T&& declval();
template <typename T> struct remove_ref { using type = T; };
template <typename T> struct remove_ref<T&> { using type = T; };
template <typename A, typename B> struct pair { A first; B second; };
template <typename... Ts> struct list {};
template <int N> struct count { static constexpr int value = count<N - 1>::value + 1; };
template <> struct count<0> { static constexpr int value = 0; };
template <typename T> struct box {
    T item;
    T& get();
    const T& get() const;
    box& self();
    T* operator->();
    T& operator*();
    T& operator[](int);
    T* begin();
    T* end();
    template <typename U> auto make(U u) -> pair<T, U>;
};
template <typename T> T call(T);
template <typename T, typename U> auto call(T t, U u) -> decltype(t.get());
int call(int);
}  // namespace fz
)code";

    std::mt19937_64 random;

    /// What each class template declared so far takes, by position: 't' a
    /// type, 'n' an int, 'b' a bool, 'p' a pack, 'd' a defaulted type.
    llvm::SmallVector<std::string> shapes;

    /// Whether the last of `shapes` is the template being declared.
    bool defining = false;
};

/// A count from the environment variable `name`, else `fallback`.
std::uint64_t environment(const char* name, std::uint64_t fallback) {
    auto* text = std::getenv(name);
    std::uint64_t value = 0;
    if(!text || llvm::StringRef(text).getAsInteger(10, value)) {
        return fallback;
    }
    return value;
}

ZEST_SUITE(TemplateResolver, Tester) {

ZEST_CASE(CallArityOccurrences) {
    /// The production occurrence path must apply the arity filter: the
    /// two-argument overload never appears for a one-argument call.
    add_focus("main.cpp", R"code(
        template <typename T>
        struct S {
            int foo(int);
            int foo(int, int);
            int foo(T);

            void call(T t) {
                foo(t);
            }
        };
    )code");
    ZASSERT(compile());

    auto semantics = Semantics::build(*unit);
    auto entries = semantics.node_entries();

    bool found = false;
    for(std::uint32_t i = 0; i < entries.size(); i += 1) {
        if(entries[i].node.get<clang::UnresolvedMemberExpr>()) {
            auto occurrences =
                resolve_occurrences(semantics, i, &unit->get<types::TemplateResolver>());
            ZEXPECT(occurrences.size() == 2);
            found = true;
        }
    }
    ZEXPECT(found);
}

ZEST_CASE(BrokenCodeScan) {
    /// Error-recovery ASTs are the resolver's everyday hostile input in an
    /// LSP; resolving anything in a broken TU must degrade, never crash.
    add_focus("main.cpp", R"code(
        template <typename T>
        struct A {
            using type = typename T::missing;
            using bad = typename unknown_ns::thing<T>::nested;
            using worse = typename A<typename T::gone>::type;
            T member;

            void f() {
                auto x = undeclared_function(member);
                member.no_such_member();
                member.get()[0][1].leaf;
            }
        };

        template <typename T>
        struct A<T*>;

        template <typename... Ts>
        struct B {
            using type = typename A<mistyped<Ts...>>::type;
            using other = typename B::unknown;
        };

        struct C : A<not_a_type> {
            using inherited = typename A<not_a_type>::type;
        };
    )code");
    prepare();
    ZASSERT(try_compile());

    Watchdog watchdog(chrono::seconds(30));
    Scanner scanner{.resolver = unit->get<types::TemplateResolver>(), .watchdog = watchdog};
    scanner.scan(*unit);
    ZEXPECT(scanner.queries > 0);
}

ZEST_CASE(StandardScan) {
    /// Every query on every dependent declaration and expression of the
    /// host's standard library, the densest dependent code a unit holds.
    add_focus("main.cpp", R"code(
        #include <algorithm>
        #include <functional>
        #include <map>
        #include <memory>
        #include <optional>
        #include <string>
        #include <tuple>
        #include <unordered_map>
        #include <variant>
        #include <vector>
    )code");
    ZASSERT(compile_driver());

    Watchdog watchdog(chrono::seconds(30));
    Scanner scanner{.resolver = unit->get<types::TemplateResolver>(), .watchdog = watchdog};
    scanner.scan(*unit);
    ZEXPECT(scanner.queries > 0);
}

ZEST_CASE(GeneratedPrograms) {
    /// Random dependent code (Generator), seeds from CLICE_RESOLVER_FUZZ_SEED
    /// on, CLICE_RESOLVER_FUZZ_RUNS of them: a longer run is one environment
    /// variable away, and a failing seed reproduces alone.
    auto first = environment("CLICE_RESOLVER_FUZZ_SEED", 0);
    auto runs = environment("CLICE_RESOLVER_FUZZ_RUNS", 256);
    Watchdog watchdog(chrono::seconds(30));
    std::uint64_t queries = 0;
    for(auto seed = first; seed < first + runs; seed += 1) {
        auto text = Generator(seed).program();
        LOG_DEBUG("template resolver fuzz: seed {}\n{}", seed, text);
        watchdog.about(std::format("seed {}:\n{}", seed, text));
        clear();
        add_focus("main.cpp", text);
        prepare();
        /// A template recursing into itself through concrete arguments is
        /// clang's to instantiate, and at its default depth of 1024 clang
        /// itself runs out of stack or time; the resolver is what is tested.
        params.input.arguments.insert(params.input.arguments.begin() + 2, "-ftemplate-depth=16");
        /// A program clang gives up on before its AST is complete has
        /// nothing to scan.
        if(!try_compile()) {
            continue;
        }
        Scanner scanner{.resolver = unit->get<types::TemplateResolver>(), .watchdog = watchdog};
        scanner.scan(*unit);
        queries += scanner.queries;
    }
    auto [slowest, kind] = watchdog.slowest_query();
    LOG_INFO("template resolver fuzz: {} programs from seed {}, {} queries, slowest {} ms ({})",
             runs,
             first,
             queries,
             slowest.count(),
             kind);
    ZEXPECT(queries > 0);
}

ZEST_CASE(ScopedBindingStaysLocal) {
    add_focus("main.cpp", R"code(
        template <typename T>
        struct Holder {
            template <typename U>
            struct Inner {
                using type = T;
            };

            template <typename U>
            void reset() {
                auto first = typename Inner<U>::type();
                using input = typename Inner<U>::type;
            }
        };

        template struct Holder<int>;
    )code");
    ZASSERT(compile());

    struct VariableFinder : clang::RecursiveASTVisitor<VariableFinder> {
        const clang::VarDecl* first = nullptr;

        bool VisitVarDecl(const clang::VarDecl* decl) {
            if(decl->getName() == "first") {
                first = decl;
            }
            return true;
        }
    } variables;

    variables.TraverseAST(unit->context());
    ZASSERT(variables.first);

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());
    ZASSERT(!finder.input.isNull());

    auto& resolver = unit->get<types::TemplateResolver>();
    auto deduced = resolver.deduce(variables.first);
    ZASSERT(!deduced.isNull());
    ZEXPECT(deduced.getCanonicalType() == unit->context().IntTy);

    auto input = resolver.resolve(finder.input);
    ZASSERT(!input.isNull());
    ZEXPECT(input->isDependentType());
}

};  // ZEST_SUITE(TemplateResolver)

}  // namespace

}  // namespace clice::testing
