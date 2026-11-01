module clice;

import :compile.compilation;
import :feature.code_completion.item;
import :feature.code_completion.whole_buffer;

namespace clice::feature {

namespace {

/// The dependent expression at the completion point, in the innermost
/// function holding it (AtPoint).
struct Finder : clang::RecursiveASTVisitor<Finder> {
    Finder(const clang::SourceManager& sources, std::uint32_t target) :
        sources(sources), target(target) {}

    bool at(clang::SourceLocation loc) {
        auto [file, offset] = sources.getDecomposedLoc(loc);
        return file == sources.getMainFileID() && offset == target;
    }

    bool holds(const clang::Stmt* body) {
        if(!body) {
            return false;
        }
        auto begin = sources.getDecomposedLoc(sources.getFileLoc(body->getBeginLoc()));
        auto end = sources.getDecomposedLoc(sources.getFileLoc(body->getEndLoc()));
        return begin.first == sources.getMainFileID() && begin.second <= target &&
               target <= end.second;
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
        if(at(sources.getFileLoc(access->getMemberLoc()))) {
            member = access;
        }
        return !found();
    }

    bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* name) {
        if(at(sources.getFileLoc(name->getLocation()))) {
            qualifier = name->getQualifierLoc();
        }
        return !found();
    }

    bool VisitDependentNameTypeLoc(clang::DependentNameTypeLoc name) {
        if(at(sources.getFileLoc(name.getNameLoc()))) {
            qualifier = name.getQualifierLoc();
        }
        return !found();
    }

    bool found() const {
        return member || qualifier;
    }

    const clang::SourceManager& sources;

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

auto WholeBuffer::instantiated(const clang::NamedDecl* templated, bool loop)
    -> std::optional<AtPoint> {
    if(!compiled) {
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
        llvm::StringRef text = input.buffers.find(input.focus)->second;
        if(auto* name = named->getIdentifier()) {
            if(!loop && !text.substr(point).contains(name->getName())) {
                return std::nullopt;
            }
            names.emplace().insert(name->getName());
        } else if(method && method->getParent()->isLambda() &&
                  method->getParent()->getParentFunctionOrMethod()) {
            names.emplace();
        }
        std::optional<CompilationParams::Instantiating> instantiating;
        if(names) {
            instantiating =
                CompilationParams::Instantiating{.point = point, .names = std::move(*names)};
        }
        compiled = compile({
            .kind = CompilationKind::Content,
            .input = input,
            .instantiating = std::move(instantiating),
            .stop = stop,
        });
    }
    auto* whole = unit();
    if(!whole) {
        return std::nullopt;
    }
    Finder finder(whole->context().getSourceManager(), target);
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

CompilationUnit* WholeBuffer::unit() {
    return compiled && compiled->completed() ? compiled.get() : nullptr;
}

}  // namespace clice::feature
