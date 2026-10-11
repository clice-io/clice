module;

#include "modules/prelude.h"
/// Ported from clang-tidy's utils/DesignatedInitializers.cpp
/// (llvmorg-23.1.2), part of the LLVM project, licensed under Apache
/// License v2.0 with LLVM Exceptions. See https://llvm.org/LICENSE.txt for
/// license information. The original leaves the base iterators of a C
/// record uninitialized, so C structs got no designators in some builds.

module clice;

import :semantic.designators;

namespace clice::designators {

namespace {

/// Whether `name` is reserved, like `_Foo` or `__elems`.
bool is_reserved(llvm::StringRef name) {
    return name.size() >= 2 && name[0] == '_' && (clang::isUppercase(name[1]) || name[1] == '_');
}

/// The designator names of an aggregate's direct subobjects, in order:
/// `[0]`, `[1]`... for an array; nothing for each base, then `.field`...
/// for a class.
class SubobjectNames {
public:
    explicit SubobjectNames(clang::QualType type) {
        if(type.isNull()) {
            return;
        }
        type = type.getCanonicalType();
        if(type->isArrayType()) {
            is_array = true;
            valid = true;
            return;
        }
        const auto* record = type->getAsRecordDecl();
        if(!record) {
            return;
        }
        valid = true;
        field = record->field_begin();
        fields_end = record->field_end();
        if(const auto* cxx_record = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
            base = cxx_record->bases_begin();
            bases_end = cxx_record->bases_end();
            valid = cxx_record->isAggregate();
        }
        one_field =
            valid && base == bases_end && field != fields_end && std::next(field) == fields_end;
    }

    explicit operator bool() const {
        return valid;
    }

    void next() {
        if(is_array) {
            index += 1;
        } else if(base != bases_end) {
            ++base;
        } else if(field != fields_end) {
            ++field;
        }
    }

    /// Appends the current subobject's designator to `out`; false when it
    /// has none. A subobject initialized through elided braces may be named
    /// by its own members instead (an anonymous struct, `std::array`'s one
    /// reserved field).
    bool append(std::string& out, bool brace_elided) {
        if(is_array) {
            out += std::format("[{}]", index);
            return true;
        }
        if(base != bases_end || field == fields_end) {
            return false;
        }
        llvm::StringRef name;
        if(const auto* identifier = field->getIdentifier()) {
            name = identifier->getName();
        }
        if(brace_elided &&
           (field->isAnonymousStructOrUnion() || (one_field && is_reserved(name)))) {
            return true;
        }
        if(name.empty() || is_reserved(name)) {
            return false;
        }
        out += '.';
        out += name;
        return true;
    }

private:
    bool valid = false;
    bool is_array = false;
    bool one_field = false;
    unsigned index = 0;
    clang::CXXRecordDecl::base_class_const_iterator base = nullptr;
    clang::CXXRecordDecl::base_class_const_iterator bases_end = nullptr;
    clang::RecordDecl::field_iterator field;
    clang::RecordDecl::field_iterator fields_end;
};

/// The designators of the subobjects `semantic` initializes, descending
/// into the ones whose braces were elided — those share the written list —
/// but not into written nested lists, which get a call of their own.
void collect(const clang::InitListExpr* semantic,
             llvm::DenseMap<clang::SourceLocation, std::string>& out,
             const llvm::DenseSet<clang::SourceLocation>& nested_braces,
             std::string& prefix) {
    if(!semantic || semantic->isTransparent()) {
        return;
    }

    SubobjectNames names(semantic->getType());
    if(!names) {
        return;
    }
    for(const clang::Expr* init: semantic->inits()) {
        auto size = prefix.size();
        auto advance = llvm::make_scope_exit([&] {
            names.next();
            prefix.resize(size);
        });
        // A broken initializer, or a hole left for value initialization.
        if(!init || llvm::isa<clang::ImplicitValueInitExpr>(init)) {
            continue;
        }

        const auto* elided = llvm::dyn_cast<clang::InitListExpr>(init);
        if(elided && nested_braces.contains(elided->getLBraceLoc())) {
            elided = nullptr;
        }
        if(!names.append(prefix, elided != nullptr)) {
            continue;
        }
        if(elided) {
            collect(elided, out, nested_braces, prefix);
            continue;
        }
        out.try_emplace(init->getBeginLoc(), prefix);
    }
}

}  // namespace

auto unwritten(const clang::InitListExpr* syntactic)
    -> llvm::DenseMap<clang::SourceLocation, std::string> {
    // InitListExpr::isExplicit() cannot tell the written nested lists of the
    // semantic form; their opening braces in the syntactic one can.
    llvm::DenseSet<clang::SourceLocation> nested_braces;
    for(const clang::Expr* init: syntactic->inits()) {
        if(const auto* nested = llvm::dyn_cast<clang::InitListExpr>(init)) {
            nested_braces.insert(nested->getLBraceLoc());
        }
    }

    llvm::DenseMap<clang::SourceLocation, std::string> designators;
    std::string prefix;
    collect(syntactic->isSemanticForm() ? syntactic : syntactic->getSemanticForm(),
            designators,
            nested_braces,
            prefix);
    return designators;
}

}  // namespace clice::designators
