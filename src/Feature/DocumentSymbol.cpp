#include "Feature/DocumentSymbol.h"

#include "AST/FilterASTVisitor.h"
#include "AST/Utility.h"
#include "Compiler/Compilation.h"
#include "Support/Compare.h"
#include "Support/Ranges.h"

namespace clice::feature {

namespace {

std::string symbol_detail(clang::ASTContext& Ctx, const clang::NamedDecl& ND) {
    clang::PrintingPolicy policy(Ctx.getPrintingPolicy());
    policy.SuppressScope = true;
    policy.SuppressUnwrittenScope = true;
    policy.AnonymousTagLocations = false;
    policy.PolishForDeclaration = true;

    std::string detail;
    llvm::raw_string_ostream os(detail);
    if(ND.getDescribedTemplateParams()) {
        os << "template ";
    }

    if(const auto* VD = dyn_cast<clang::ValueDecl>(&ND)) {
        // FIXME: better printing for dependent type
        if(isa<clang::CXXConstructorDecl>(VD)) {
            std::string type = VD->getType().getAsString(policy);
            // Print constructor type as "(int)" instead of "void (int)".
            llvm::StringRef without_void = type;
            without_void.consume_front("void ");
            os << without_void;
        } else if(!isa<clang::CXXDestructorDecl>(VD)) {
            VD->getType().print(os, policy);
        }
    } else if(const auto* TD = dyn_cast<clang::TagDecl>(&ND)) {
        os << TD->getKindName();
    } else if(isa<clang::TypedefNameDecl>(&ND)) {
        os << "type alias";
    } else if(isa<clang::ConceptDecl>(&ND)) {
        os << "concept";
    }

    return detail;
}

/// Use DFS to traverse the AST and collect document symbols.
class DocumentSymbolCollector : public FilteredASTVisitor<DocumentSymbolCollector> {
public:
    using Base = FilteredASTVisitor<DocumentSymbolCollector>;

    DocumentSymbolCollector(CompilationUnit& unit, bool interested_only) :
        Base(unit, interested_only) {}

    bool is_interested(clang::Decl* decl) {
        switch(decl->getKind()) {
            case clang::Decl::Namespace:
            case clang::Decl::Enum:
            case clang::Decl::EnumConstant:
            case clang::Decl::Function:
            case clang::Decl::CXXMethod:
            case clang::Decl::CXXConstructor:
            case clang::Decl::CXXDestructor:
            case clang::Decl::CXXConversion:
            case clang::Decl::CXXDeductionGuide:
            case clang::Decl::Record:
            case clang::Decl::CXXRecord:
            case clang::Decl::Field:
            case clang::Decl::Var:
            case clang::Decl::Binding:
            case clang::Decl::Concept: {
                return true;
            }

            default: {
                return false;
            }
        }
    }

    bool on_traverse_decl(clang::Decl* decl, auto MF) {
        if(!is_interested(decl)) {
            return (this->*MF)(decl);
        }

        auto ND = llvm::cast<clang::NamedDecl>(decl);
        auto [fid, selection_range] =
            unit.decompose_range(unit.expansion_location(ND->getLocation()));
        auto [fid2, range] = unit.decompose_expansion_range(ND->getSourceRange());
        if(fid != fid2) {
            return true;
        }

        auto& frame = interested_only ? result : sharedResult[fid];
        auto cursor = frame.cursor;

        /// Add new symbol.
        auto& symbol = frame.cursor->emplace_back();
        symbol.kind = SymbolKind::from(decl);
        symbol.name = ast::display_name_of(ND);
        symbol.detail = symbol_detail(unit.context(), *ND);
        symbol.selectionRange = selection_range;
        symbol.range = range;

        /// Adjust the node.
        frame.cursor = &symbol.children;

        bool res = (this->*MF)(decl);

        /// When all children node are set, go back to last node.
        (interested_only ? result : sharedResult[fid]).cursor = cursor;

        return res;
    }

public:
    struct SymbolFrame {
        DocumentSymbols symbols;
        DocumentSymbols* cursor = &symbols;
    };

    SymbolFrame result;
    index::Shared<SymbolFrame> sharedResult;
};

}  // namespace

DocumentSymbols document_symbols(CompilationUnit& unit) {
    DocumentSymbolCollector collector(unit, true);
    collector.TraverseDecl(unit.tu());

    auto& frame = collector.result;
    ranges::sort(frame.symbols, refl::less);
    return std::move(frame.symbols);
}

index::Shared<DocumentSymbols> index_document_symbol(CompilationUnit& unit) {
    DocumentSymbolCollector collector(unit, true);
    collector.TraverseDecl(unit.tu());

    index::Shared<DocumentSymbols> result;
    for(auto& [fid, frame]: collector.sharedResult) {
        ranges::sort(frame.symbols, refl::less);
        result.try_emplace(fid, std::move(frame.symbols));
    }
    return result;
}

}  // namespace clice::feature
