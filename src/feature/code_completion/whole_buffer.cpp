module clice;

import :compile.compilation;
import :feature.code_completion.cursor;
import :feature.code_completion.whole_buffer;

namespace clice::feature {

namespace {

/// The dependent expression at the completion point, in the innermost
/// function holding it (AtPoint).
struct Finder : clang::RecursiveASTVisitor<Finder> {
    Finder(CompilationUnit& unit, std::uint32_t target) : unit(unit), target(target) {}

    bool at(clang::SourceLocation loc) {
        return unit.focus_file().offset(loc) == target;
    }

    bool holds(const clang::Stmt* body) {
        if(!body) {
            return false;
        }
        auto begin = unit.focus_file().offset(body->getBeginLoc());
        return begin && *begin <= target && target <= unit.seen_at(body->getEndLoc()).offset;
    }

    bool VisitFunctionDecl(clang::FunctionDecl* function) {
        if(holds(function->getBody())) {
            context = function;
        }
        return true;
    }

    bool VisitLambdaExpr(clang::LambdaExpr* lambda) {
        if(holds(lambda->getBody())) {
            context = lambda->getCallOperator();
        }
        return true;
    }

    bool VisitCXXDependentScopeMemberExpr(clang::CXXDependentScopeMemberExpr* access) {
        if(at(access->getMemberLoc())) {
            member = access;
        }
        return !found();
    }

    bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* name) {
        if(at(name->getLocation())) {
            qualifier = name->getQualifierLoc();
        }
        return !found();
    }

    bool VisitDependentNameTypeLoc(clang::DependentNameTypeLoc name) {
        if(at(name.getNameLoc())) {
            qualifier = name.getQualifierLoc();
        }
        return !found();
    }

    bool found() const {
        return member || qualifier;
    }

    CompilationUnit& unit;

    std::uint32_t target;

    clang::FunctionDecl* context = nullptr;

    clang::CXXDependentScopeMemberExpr* member = nullptr;

    clang::NestedNameSpecifierLoc qualifier;
};

}  // namespace

WholeBuffer::WholeBuffer(const CompilationParams& completion) :
    input(completion.input), point(completion.completion.offset), stop(completion.stop) {
    auto& text = input.buffers.find(input.focus)->second;
    auto whole = Prefix::from(text, point).whole;
    target = whole.begin;
    if(whole.length() == 0) {
        text.insert(point, " __clice_completion");
        target = point + 1;
    }
}

auto WholeBuffer::instantiated(const clang::NamedDecl* templated,
                               bool loop,
                               CompilationUnit& completion) -> std::optional<AtPoint> {
    if(!compiled) {
        // The resolver binds no class template's parameters through its
        // instantiations: a member of one learns nothing more here, but for
        // the statement at the point.
        bool member = llvm::isa<clang::CXXRecordDecl>(templated);
        if(member && !loop) {
            return std::nullopt;
        }
        // A call after the point names the template it calls, a constructor
        // its class; one nothing calls yet goes without the compile. A lambda
        // in a function is called in the body holding the point; any body may
        // call an operator, a conversion or a lambda elsewhere.
        std::optional<llvm::StringSet<>> names;
        const auto* named = templated;
        if(auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(templated)) {
            named = constructor->getParent();
        }
        auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(templated);
        if(auto* name = named->getIdentifier()) {
            if(!loop && !called_after(name->getName(), completion.sema().getPreprocessor())) {
                return std::nullopt;
            }
            names.emplace().insert(name->getName());
        } else if(method && method->getParent()->isLambda() &&
                  method->getParent()->getParentFunctionOrMethod()) {
            names.emplace();
        }
        std::optional<std::uint32_t> declared;
        if(!member) {
            declared = completion.produced_at(templated->getLocation()).offset;
        }
        compiled = compile({
            .kind = CompilationKind::Content,
            .input = input,
            .instantiating =
                CompilationParams::Instantiating{
                                                 .point = point,
                                                 .names = std::move(names),
                                                 .templated = declared,
                                                 },
            .stop = stop,
        });
    }
    auto* whole = unit();
    if(!whole) {
        return std::nullopt;
    }
    Finder finder(*whole, target);
    for(auto* decl: whole->top_level_decls()) {
        if(!finder.TraverseDecl(decl)) {
            break;
        }
    }
    if(!finder.found() || !finder.context) {
        return std::nullopt;
    }
    return AtPoint{
        .unit = *whole,
        .context = finder.context,
        .member = finder.member,
        .qualifier = finder.qualifier,
    };
}

bool WholeBuffer::called_after(llvm::StringRef name, clang::Preprocessor& pp) {
    auto& text = input.buffers.find(input.focus)->second;
    clang::Lexer lexer({},
                       pp.getLangOpts(),
                       text.data(),
                       text.data() + point,
                       text.data() + text.size());
    auto spells = [&](llvm::StringRef word) {
        return word == name;
    };
    llvm::DenseMap<const clang::MacroInfo*, bool> read;
    clang::Token token;
    for(bool end = false; !end;) {
        end = lexer.LexFromRawLexer(token);
        if(token.is(clang::tok::raw_identifier) &&
           (spells(token.getRawIdentifier()) ||
            macro_spells(pp, token.getRawIdentifier(), spells, read))) {
            return true;
        }
    }
    return false;
}

CompilationUnit* WholeBuffer::unit() {
    return compiled && compiled->completed() ? compiled.get() : nullptr;
}

}  // namespace clice::feature
