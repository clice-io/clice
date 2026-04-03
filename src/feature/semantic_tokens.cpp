#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "feature/feature.h"
#include "semantic/ast_utility.h"
#include "semantic/semantic_visitor.h"
#include "semantic/symbol_kind.h"
#include "syntax/lexer.h"

#include "clang/AST/Attr.h"
#include "clang/Basic/IdentifierTable.h"

namespace clice::feature {

namespace {

struct RawToken {
    LocalSourceRange range;
    SymbolKind kind = SymbolKind::Invalid;
    std::uint32_t modifiers = 0;
};

constexpr std::uint32_t bit(SymbolModifiers::Kind kind) {
    return static_cast<std::uint32_t>(kind);
}

void add_modifier(std::uint32_t& modifiers, SymbolModifiers::Kind kind) {
    modifiers |= bit(kind);
}

auto type_index(SymbolKind kind) -> std::uint32_t {
    return kind.value_of();
}

auto encode_modifiers(std::uint32_t modifiers) -> std::uint32_t {
    return modifiers;
}

class SemanticTokensCollector : public SemanticVisitor<SemanticTokensCollector> {
public:
    explicit SemanticTokensCollector(CompilationUnitRef unit) : SemanticVisitor(unit, true) {}

    auto collect() -> std::vector<RawToken> {
        highlight_lexical(unit.interested_file());
        run();
        merge_tokens();
        return std::move(tokens);
    }

    void handleDeclOccurrence(const clang::NamedDecl* decl,
                              RelationKind relation,
                              clang::SourceLocation location) {
        std::uint32_t modifiers = 0;
        if(relation.is_one_of(RelationKind::Definition)) {
            add_modifier(modifiers, SymbolModifiers::Definition);
        } else if(relation.is_one_of(RelationKind::Declaration)) {
            add_modifier(modifiers, SymbolModifiers::Declaration);
        }

        if(ast::is_templated(decl)) {
            add_modifier(modifiers, SymbolModifiers::Templated);
        }

        add_token(location, SymbolKind::from(decl), modifiers);
    }

    void handleMacroOccurrence(const clang::MacroInfo*,
                               RelationKind relation,
                               clang::SourceLocation location) {
        std::uint32_t modifiers = 0;
        if(relation.is_one_of(RelationKind::Definition)) {
            add_modifier(modifiers, SymbolModifiers::Definition);
        } else if(relation.is_one_of(RelationKind::Declaration)) {
            add_modifier(modifiers, SymbolModifiers::Declaration);
        }

        add_token(location, SymbolKind::Macro, modifiers);
    }

    void handleAttrOccurrence(const clang::Attr* attr, clang::SourceRange range) {
        auto [begin, end] = range;
        if(llvm::isa<clang::FinalAttr, clang::OverrideAttr>(attr)) {
            assert(begin == end && "attribute token should be one location");
            add_token(begin, SymbolKind::Keyword, 0);
        }
    }

private:
    void add_token(clang::FileID fid, Token token, SymbolKind kind, std::uint32_t modifiers) {
        if(fid != unit.interested_file() || kind == SymbolKind::Invalid) {
            return;
        }

        tokens.push_back({
            .range = token.range,
            .kind = kind,
            .modifiers = modifiers,
        });
    }

    void add_token(clang::SourceLocation location, SymbolKind kind, std::uint32_t modifiers) {
        if(kind == SymbolKind::Invalid) {
            return;
        }

        if(location.isMacroID()) {
            auto spelling = unit.spelling_location(location);
            auto expansion = unit.expansion_location(location);
            if(unit.file_id(spelling) != unit.file_id(expansion)) {
                return;
            }
            location = spelling;
        }

        auto [fid, range] = unit.decompose_range(location);
        if(fid != unit.interested_file()) {
            return;
        }

        tokens.push_back({
            .range = range,
            .kind = kind,
            .modifiers = modifiers,
        });
    }

    void highlight_lexical(clang::FileID fid) {
        auto content = unit.file_content(fid);
        auto& lang_opts = unit.lang_options();
        clang::IdentifierTable identifiers(lang_opts);
        Lexer lexer(content, false, &lang_opts);

        while(true) {
            Token token = lexer.advance();
            if(token.is_eof()) {
                break;
            }

            SymbolKind kind = SymbolKind::Invalid;

            if(token.is_directive_hash() || token.is_pp_keyword) {
                kind = SymbolKind::Directive;
            } else {
                switch(token.kind) {
                    case clang::tok::comment: kind = SymbolKind::Comment; break;
                    case clang::tok::numeric_constant: kind = SymbolKind::Number; break;
                    case clang::tok::char_constant:
                    case clang::tok::wide_char_constant:
                    case clang::tok::utf8_char_constant:
                    case clang::tok::utf16_char_constant:
                    case clang::tok::utf32_char_constant: kind = SymbolKind::Character; break;
                    case clang::tok::string_literal:
                    case clang::tok::wide_string_literal:
                    case clang::tok::utf8_string_literal:
                    case clang::tok::utf16_string_literal:
                    case clang::tok::utf32_string_literal: kind = SymbolKind::String; break;
                    case clang::tok::header_name: kind = SymbolKind::Header; break;
                    case clang::tok::identifier: break;
                    case clang::tok::raw_identifier: {
                        auto previous = lexer.last();
                        if(previous.is_pp_keyword && previous.text(content) == "define") {
                            kind = SymbolKind::Macro;
                            break;
                        }

                        auto spelling = token.text(content);
                        if(identifiers.get(spelling).isKeyword(lang_opts)) {
                            kind = SymbolKind::Keyword;
                        }
                        break;
                    }
                    /* Keywords */
                    case clang::tok::kw_auto:
                    case clang::tok::kw_break:
                    case clang::tok::kw_case:
                    case clang::tok::kw_char:
                    case clang::tok::kw_const:
                    case clang::tok::kw_continue:
                    case clang::tok::kw_default:
                    case clang::tok::kw_do:
                    case clang::tok::kw_double:
                    case clang::tok::kw_else:
                    case clang::tok::kw_enum:
                    case clang::tok::kw_extern:
                    case clang::tok::kw_float:
                    case clang::tok::kw_for:
                    case clang::tok::kw_goto:
                    case clang::tok::kw_if:
                    case clang::tok::kw_int:
                    case clang::tok::kw__ExtInt:
                    case clang::tok::kw__BitInt:
                    case clang::tok::kw_long:
                    case clang::tok::kw_register:
                    case clang::tok::kw_return:
                    case clang::tok::kw_short:
                    case clang::tok::kw_signed:
                    case clang::tok::kw_sizeof:
                    case clang::tok::kw___datasizeof:
                    case clang::tok::kw_static:
                    case clang::tok::kw_struct:
                    case clang::tok::kw_switch:
                    case clang::tok::kw_typedef:
                    case clang::tok::kw_union:
                    case clang::tok::kw_unsigned:
                    case clang::tok::kw_void:
                    case clang::tok::kw_volatile:
                    case clang::tok::kw_while:
                    case clang::tok::kw__Alignas:
                    case clang::tok::kw__Alignof:
                    case clang::tok::kw__Atomic:
                    case clang::tok::kw__Bool:
                    case clang::tok::kw__Complex:
                    case clang::tok::kw__Generic:
                    case clang::tok::kw__Imaginary:
                    case clang::tok::kw__Noreturn:
                    case clang::tok::kw__Static_assert:
                    case clang::tok::kw__Thread_local:
                    case clang::tok::kw___func__:
                    case clang::tok::kw___objc_yes:
                    case clang::tok::kw___objc_no:
                    case clang::tok::kw___ptrauth:
                    case clang::tok::kw__Countof:
                    case clang::tok::kw_asm:
                    case clang::tok::kw_bool:
                    case clang::tok::kw_catch:
                    case clang::tok::kw_class:
                    case clang::tok::kw_const_cast:
                    case clang::tok::kw_delete:
                    case clang::tok::kw_dynamic_cast:
                    case clang::tok::kw_explicit:
                    case clang::tok::kw_export:
                    case clang::tok::kw_false:
                    case clang::tok::kw_friend:
                    case clang::tok::kw_mutable:
                    case clang::tok::kw_namespace:
                    case clang::tok::kw_new:
                    case clang::tok::kw_operator:
                    case clang::tok::kw_private:
                    case clang::tok::kw_protected:
                    case clang::tok::kw_public:
                    case clang::tok::kw_reinterpret_cast:
                    case clang::tok::kw_static_cast:
                    case clang::tok::kw_template:
                    case clang::tok::kw_this:
                    case clang::tok::kw_throw:
                    case clang::tok::kw_true:
                    case clang::tok::kw_try:
                    case clang::tok::kw_typename:
                    case clang::tok::kw_typeid:
                    case clang::tok::kw_using:
                    case clang::tok::kw_virtual:
                    case clang::tok::kw_wchar_t:
                    case clang::tok::kw_restrict:
                    case clang::tok::kw_inline:
                    case clang::tok::kw_alignas:
                    case clang::tok::kw_alignof:
                    case clang::tok::kw_char16_t:
                    case clang::tok::kw_char32_t:
                    case clang::tok::kw_constexpr:
                    case clang::tok::kw_decltype:
                    case clang::tok::kw_noexcept:
                    case clang::tok::kw_nullptr:
                    case clang::tok::kw_static_assert:
                    case clang::tok::kw_thread_local:
                    case clang::tok::kw_co_await:
                    case clang::tok::kw_co_return:
                    case clang::tok::kw_co_yield:
                    case clang::tok::kw_module:
                    case clang::tok::kw_import:
                    case clang::tok::kw_consteval:
                    case clang::tok::kw_constinit:
                    case clang::tok::kw_concept:
                    case clang::tok::kw_requires:
                    case clang::tok::kw_char8_t:
                    case clang::tok::kw__Float16:
                    case clang::tok::kw_typeof:
                    case clang::tok::kw_typeof_unqual:
                    case clang::tok::kw__Accum:
                    case clang::tok::kw__Fract:
                    case clang::tok::kw__Sat:
                    case clang::tok::kw__Decimal32:
                    case clang::tok::kw__Decimal64:
                    case clang::tok::kw__Decimal128:
                    case clang::tok::kw___null:
                    case clang::tok::kw___alignof:
                    case clang::tok::kw___attribute:
                    case clang::tok::kw___builtin_choose_expr:
                    case clang::tok::kw___builtin_offsetof:
                    case clang::tok::kw___builtin_FILE:
                    case clang::tok::kw___builtin_FILE_NAME:
                    case clang::tok::kw___builtin_FUNCTION:
                    case clang::tok::kw___builtin_FUNCSIG:
                    case clang::tok::kw___builtin_LINE:
                    case clang::tok::kw___builtin_COLUMN:
                    case clang::tok::kw___builtin_source_location:
                    case clang::tok::kw___builtin_types_compatible_p:
                    case clang::tok::kw___builtin_va_arg:
                    case clang::tok::kw___extension__:
                    case clang::tok::kw___float128:
                    case clang::tok::kw___ibm128:
                    case clang::tok::kw___imag:
                    case clang::tok::kw___int128:
                    case clang::tok::kw___label__:
                    case clang::tok::kw___real:
                    case clang::tok::kw___thread:
                    case clang::tok::kw___FUNCTION__:
                    case clang::tok::kw___PRETTY_FUNCTION__:
                    case clang::tok::kw___auto_type:
                    case clang::tok::kw___FUNCDNAME__:
                    case clang::tok::kw___FUNCSIG__:
                    case clang::tok::kw_L__FUNCTION__:
                    case clang::tok::kw_L__FUNCSIG__:
                    case clang::tok::kw___is_interface_class:
                    case clang::tok::kw___is_sealed:
                    case clang::tok::kw___is_destructible:
                    case clang::tok::kw___is_trivially_destructible:
                    case clang::tok::kw___is_nothrow_destructible:
                    case clang::tok::kw___is_nothrow_assignable:
                    case clang::tok::kw___is_constructible:
                    case clang::tok::kw___is_nothrow_constructible:
                    case clang::tok::kw___is_assignable:
                    case clang::tok::kw___has_nothrow_move_assign:
                    case clang::tok::kw___has_trivial_move_assign:
                    case clang::tok::kw___has_trivial_move_constructor:
                    case clang::tok::kw___builtin_is_implicit_lifetime:
                    case clang::tok::kw___builtin_is_virtual_base_of:
                    case clang::tok::kw___has_nothrow_assign:
                    case clang::tok::kw___has_nothrow_copy:
                    case clang::tok::kw___has_nothrow_constructor:
                    case clang::tok::kw___has_trivial_assign:
                    case clang::tok::kw___has_trivial_copy:
                    case clang::tok::kw___has_trivial_constructor:
                    case clang::tok::kw___has_trivial_destructor:
                    case clang::tok::kw___has_virtual_destructor:
                    case clang::tok::kw___is_abstract:
                    case clang::tok::kw___is_aggregate:
                    case clang::tok::kw___is_base_of:
                    case clang::tok::kw___is_class:
                    case clang::tok::kw___is_convertible_to:
                    case clang::tok::kw___is_empty:
                    case clang::tok::kw___is_enum:
                    case clang::tok::kw___is_final:
                    case clang::tok::kw___is_literal:
                    case clang::tok::kw___is_pod:
                    case clang::tok::kw___is_polymorphic:
                    case clang::tok::kw___is_standard_layout:
                    case clang::tok::kw___is_trivial:
                    case clang::tok::kw___is_trivially_assignable:
                    case clang::tok::kw___is_trivially_constructible:
                    case clang::tok::kw___is_trivially_copyable:
                    case clang::tok::kw___is_union:
                    case clang::tok::kw___has_unique_object_representations:
                    case clang::tok::kw___is_layout_compatible:
                    case clang::tok::kw___is_pointer_interconvertible_base_of:
                    case clang::tok::kw___add_lvalue_reference:
                    case clang::tok::kw___add_pointer:
                    case clang::tok::kw___add_rvalue_reference:
                    case clang::tok::kw___decay:
                    case clang::tok::kw___make_signed:
                    case clang::tok::kw___make_unsigned:
                    case clang::tok::kw___remove_all_extents:
                    case clang::tok::kw___remove_const:
                    case clang::tok::kw___remove_cv:
                    case clang::tok::kw___remove_cvref:
                    case clang::tok::kw___remove_extent:
                    case clang::tok::kw___remove_pointer:
                    case clang::tok::kw___remove_reference_t:
                    case clang::tok::kw___remove_restrict:
                    case clang::tok::kw___remove_volatile:
                    case clang::tok::kw___underlying_type:
                    case clang::tok::kw___is_trivially_equality_comparable:
                    case clang::tok::kw___is_bounded_array:
                    case clang::tok::kw___is_unbounded_array:
                    case clang::tok::kw___is_scoped_enum:
                    case clang::tok::kw___can_pass_in_regs:
                    case clang::tok::kw___reference_binds_to_temporary:
                    case clang::tok::kw___reference_constructs_from_temporary:
                    case clang::tok::kw___reference_converts_from_temporary:
                    case clang::tok::kw_:
                    case clang::tok::kw___builtin_is_cpp_trivially_relocatable:
                    case clang::tok::kw___is_trivially_relocatable:
                    case clang::tok::kw___is_bitwise_cloneable:
                    case clang::tok::kw___builtin_is_replaceable:
                    case clang::tok::kw___builtin_structured_binding_size:
                    case clang::tok::kw___is_lvalue_expr:
                    case clang::tok::kw___is_rvalue_expr:
                    case clang::tok::kw___is_arithmetic:
                    case clang::tok::kw___is_floating_point:
                    case clang::tok::kw___is_integral:
                    case clang::tok::kw___is_complete_type:
                    case clang::tok::kw___is_void:
                    case clang::tok::kw___is_array:
                    case clang::tok::kw___is_function:
                    case clang::tok::kw___is_reference:
                    case clang::tok::kw___is_lvalue_reference:
                    case clang::tok::kw___is_rvalue_reference:
                    case clang::tok::kw___is_fundamental:
                    case clang::tok::kw___is_object:
                    case clang::tok::kw___is_scalar:
                    case clang::tok::kw___is_compound:
                    case clang::tok::kw___is_pointer:
                    case clang::tok::kw___is_member_object_pointer:
                    case clang::tok::kw___is_member_function_pointer:
                    case clang::tok::kw___is_member_pointer:
                    case clang::tok::kw___is_const:
                    case clang::tok::kw___is_volatile:
                    case clang::tok::kw___is_signed:
                    case clang::tok::kw___is_unsigned:
                    case clang::tok::kw___is_same:
                    case clang::tok::kw___is_convertible:
                    case clang::tok::kw___is_nothrow_convertible:
                    case clang::tok::kw___array_rank:
                    case clang::tok::kw___array_extent:
                    case clang::tok::kw___private_extern__:
                    case clang::tok::kw___module_private__:
                    case clang::tok::kw___builtin_ptrauth_type_discriminator:
                    case clang::tok::kw___declspec:
                    case clang::tok::kw___cdecl:
                    case clang::tok::kw___stdcall:
                    case clang::tok::kw___fastcall:
                    case clang::tok::kw___thiscall:
                    case clang::tok::kw___regcall:
                    case clang::tok::kw___vectorcall:
                    case clang::tok::kw___forceinline:
                    case clang::tok::kw___unaligned:
                    case clang::tok::kw___super:
                    case clang::tok::kw___global:
                    case clang::tok::kw___local:
                    case clang::tok::kw___constant:
                    case clang::tok::kw___private:
                    case clang::tok::kw___generic:
                    case clang::tok::kw___kernel:
                    case clang::tok::kw___read_only:
                    case clang::tok::kw___write_only:
                    case clang::tok::kw___read_write:
                    case clang::tok::kw___builtin_astype:
                    case clang::tok::kw_vec_step:
                    case clang::tok::kw_image1d_t:
                    case clang::tok::kw_image1d_array_t:
                    case clang::tok::kw_image1d_buffer_t:
                    case clang::tok::kw_image2d_t:
                    case clang::tok::kw_image2d_array_t:
                    case clang::tok::kw_image2d_depth_t:
                    case clang::tok::kw_image2d_array_depth_t:
                    case clang::tok::kw_image2d_msaa_t:
                    case clang::tok::kw_image2d_array_msaa_t:
                    case clang::tok::kw_image2d_msaa_depth_t:
                    case clang::tok::kw_image2d_array_msaa_depth_t:
                    case clang::tok::kw_image3d_t:
                    case clang::tok::kw_pipe:
                    case clang::tok::kw_addrspace_cast:
                    case clang::tok::kw___noinline__:
                    case clang::tok::kw_cbuffer:
                    case clang::tok::kw_tbuffer:
                    case clang::tok::kw_groupshared:
                    case clang::tok::kw_in:
                    case clang::tok::kw_inout:
                    case clang::tok::kw_out:
                    case clang::tok::kw___hlsl_resource_t:
                    case clang::tok::kw___builtin_hlsl_is_scalarized_layout_compatible:
                    case clang::tok::kw___builtin_hlsl_is_intangible:
                    case clang::tok::kw___builtin_hlsl_is_typed_resource_element_compatible:
                    case clang::tok::kw___builtin_omp_required_simd_align:
                    case clang::tok::kw___pascal:
                    case clang::tok::kw___vector:
                    case clang::tok::kw___pixel:
                    case clang::tok::kw___bool:
                    case clang::tok::kw___bf16:
                    case clang::tok::kw_half:
                    case clang::tok::kw___bridge:
                    case clang::tok::kw___bridge_transfer:
                    case clang::tok::kw___bridge_retained:
                    case clang::tok::kw___bridge_retain:
                    case clang::tok::kw___covariant:
                    case clang::tok::kw___contravariant:
                    case clang::tok::kw___kindof:
                    case clang::tok::kw__Nonnull:
                    case clang::tok::kw__Nullable:
                    case clang::tok::kw__Nullable_result:
                    case clang::tok::kw__Null_unspecified:
                    case clang::tok::kw___funcref:
                    case clang::tok::kw___ptr64:
                    case clang::tok::kw___ptr32:
                    case clang::tok::kw___sptr:
                    case clang::tok::kw___uptr:
                    case clang::tok::kw___w64:
                    case clang::tok::kw___uuidof:
                    case clang::tok::kw___try:
                    case clang::tok::kw___finally:
                    case clang::tok::kw___leave:
                    case clang::tok::kw___int64:
                    case clang::tok::kw___if_exists:
                    case clang::tok::kw___if_not_exists:
                    case clang::tok::kw___single_inheritance:
                    case clang::tok::kw___multiple_inheritance:
                    case clang::tok::kw___virtual_inheritance:
                    case clang::tok::kw___interface:
                    case clang::tok::kw___builtin_convertvector:
                    case clang::tok::kw___builtin_vectorelements:
                    case clang::tok::kw___builtin_bit_cast:
                    case clang::tok::kw___builtin_available:
                    case clang::tok::kw___builtin_sycl_unique_stable_name:
                    case clang::tok::kw___arm_agnostic:
                    case clang::tok::kw___arm_in:
                    case clang::tok::kw___arm_inout:
                    case clang::tok::kw___arm_locally_streaming:
                    case clang::tok::kw___arm_new:
                    case clang::tok::kw___arm_out:
                    case clang::tok::kw___arm_preserves:
                    case clang::tok::kw___arm_streaming:
                    case clang::tok::kw___arm_streaming_compatible:
                    case clang::tok::kw___unknown_anytype: kind = SymbolKind::Keyword; break;
                    /* Operators */
                    case clang::tok::l_square:
                    case clang::tok::r_square:
                    case clang::tok::l_paren:
                    case clang::tok::r_paren:
                    case clang::tok::l_brace:
                    case clang::tok::r_brace:
                    case clang::tok::period:
                    case clang::tok::ellipsis:
                    case clang::tok::amp:
                    case clang::tok::ampamp:
                    case clang::tok::ampequal:
                    case clang::tok::star:
                    case clang::tok::starequal:
                    case clang::tok::plus:
                    case clang::tok::plusplus:
                    case clang::tok::plusequal:
                    case clang::tok::minus:
                    case clang::tok::arrow:
                    case clang::tok::minusminus:
                    case clang::tok::minusequal:
                    case clang::tok::tilde:
                    case clang::tok::exclaim:
                    case clang::tok::exclaimequal:
                    case clang::tok::slash:
                    case clang::tok::slashequal:
                    case clang::tok::percent:
                    case clang::tok::percentequal:
                    case clang::tok::less:
                    case clang::tok::lessless:
                    case clang::tok::lessequal:
                    case clang::tok::lesslessequal:
                    case clang::tok::greater:
                    case clang::tok::greatergreater:
                    case clang::tok::greaterequal:
                    case clang::tok::greatergreaterequal:
                    case clang::tok::caret:
                    case clang::tok::caretequal:
                    case clang::tok::pipe:
                    case clang::tok::pipepipe:
                    case clang::tok::pipeequal:
                    case clang::tok::question:
                    case clang::tok::colon:
                    case clang::tok::semi:
                    case clang::tok::equal:
                    case clang::tok::equalequal:
                    case clang::tok::comma:
                    case clang::tok::hashat:
                    case clang::tok::periodstar:
                    case clang::tok::arrowstar:
                    case clang::tok::coloncolon:
                    case clang::tok::at:
                    case clang::tok::lesslessless:
                    case clang::tok::greatergreatergreater: break;
                    case clang::tok::annot_cxxscope:
                    case clang::tok::annot_typename:
                    case clang::tok::annot_template_id:
                    case clang::tok::annot_non_type:
                    case clang::tok::annot_non_type_undeclared:
                    case clang::tok::annot_non_type_dependent:
                    case clang::tok::annot_overload_set:
                    case clang::tok::annot_primary_expr:
                    case clang::tok::annot_decltype:
                    case clang::tok::annot_pack_indexing_type:
                    case clang::tok::annot_pragma_unused:
                    case clang::tok::annot_pragma_vis:
                    case clang::tok::annot_pragma_pack:
                    case clang::tok::annot_pragma_parser_crash:
                    case clang::tok::annot_pragma_captured:
                    case clang::tok::annot_pragma_dump:
                    case clang::tok::annot_pragma_msstruct:
                    case clang::tok::annot_pragma_align:
                    case clang::tok::annot_pragma_weak:
                    case clang::tok::annot_pragma_weakalias:
                    case clang::tok::annot_pragma_redefine_extname:
                    case clang::tok::annot_pragma_fp_contract:
                    case clang::tok::annot_pragma_fenv_access:
                    case clang::tok::annot_pragma_fenv_access_ms:
                    case clang::tok::annot_pragma_fenv_round:
                    case clang::tok::annot_pragma_cx_limited_range:
                    case clang::tok::annot_pragma_float_control:
                    case clang::tok::annot_pragma_ms_pointers_to_members:
                    case clang::tok::annot_pragma_ms_vtordisp:
                    case clang::tok::annot_pragma_ms_pragma:
                    case clang::tok::annot_pragma_opencl_extension:
                    case clang::tok::annot_attr_openmp:
                    case clang::tok::annot_pragma_openmp:
                    case clang::tok::annot_pragma_openmp_end:
                    case clang::tok::annot_pragma_openacc:
                    case clang::tok::annot_pragma_openacc_end:
                    case clang::tok::annot_pragma_loop_hint:
                    case clang::tok::annot_pragma_fp:
                    case clang::tok::annot_pragma_attribute:
                    case clang::tok::annot_pragma_riscv:
                    case clang::tok::annot_module_include:
                    case clang::tok::annot_module_begin:
                    case clang::tok::annot_module_end:
                    case clang::tok::annot_header_unit:
                    case clang::tok::annot_repl_input_end:
                    case clang::tok::annot_embed: break;
                    /* Others */
                    case clang::tok::spaceship:
                    case clang::tok::binary_data:
                    case clang::tok::hash:
                    case clang::tok::hashhash:
                    case clang::tok::unknown:
                    case clang::tok::eof:
                    case clang::tok::eod:
                    case clang::tok::code_completion:
                    case clang::tok::NUM_TOKENS: break;
                }
            }

            add_token(fid, token, kind, 0);
        }
    }

    static void resolve_conflict(RawToken& last, const RawToken& current) {
        (void)current;
        if(last.kind == SymbolKind::Conflict) {
            return;
        }
        last.kind = SymbolKind::Conflict;
    }

    void merge_tokens() {
        std::ranges::sort(tokens, [](const RawToken& lhs, const RawToken& rhs) {
            if(lhs.range.begin != rhs.range.begin) {
                return lhs.range.begin < rhs.range.begin;
            }
            return lhs.range.end < rhs.range.end;
        });

        std::vector<RawToken> merged;
        merged.reserve(tokens.size());

        for(const auto& token: tokens) {
            if(merged.empty()) {
                merged.push_back(token);
                continue;
            }

            auto& last = merged.back();
            if(last.range == token.range) {
                resolve_conflict(last, token);
                continue;
            }

            if(last.range.end == token.range.begin && last.kind == token.kind) {
                last.range.end = token.range.end;
                continue;
            }

            merged.push_back(token);
        }

        tokens = std::move(merged);
    }

public:
    std::vector<RawToken> tokens;
};

class SemanticTokenEncoder {
public:
    SemanticTokenEncoder(llvm::StringRef content,
                         PositionEncoding encoding,
                         protocol::SemanticTokens& output) :
        content(content), converter(content, encoding), output(output) {}

    void append(const RawToken& token) {
        if(!token.range.valid() || token.range.end <= token.range.begin ||
           token.range.end > content.size()) {
            return;
        }

        auto begin = token.range.begin;
        auto end = token.range.end;
        auto begin_position = *converter.to_position(begin);
        auto end_position = *converter.to_position(end);
        auto begin_line = static_cast<std::uint32_t>(begin_position.line);
        auto begin_char = static_cast<std::uint32_t>(begin_position.character);
        auto end_line = static_cast<std::uint32_t>(end_position.line);
        auto end_char = static_cast<std::uint32_t>(end_position.character);

        if(begin_line == end_line) [[likely]] {
            auto delta_line = begin_line - last_line;
            auto delta_start = delta_line == 0 ? begin_char - last_start_character : begin_char;
            auto token_length = end_char - begin_char;
            emit_relative(delta_line, delta_start, token_length, token.kind, token.modifiers);
        } else {
            auto chunk = content.substr(begin, end - begin);
            bool first_piece = true;
            std::uint32_t chunk_offset = 0;
            std::uint32_t piece_size = 0;

            for(char c: chunk) {
                piece_size += 1;
                if(c != '\n') {
                    continue;
                }

                std::uint32_t delta_line = 1;
                std::uint32_t delta_start = 0;
                if(first_piece) {
                    delta_line = begin_line - last_line;
                    delta_start = delta_line == 0 ? begin_char - last_start_character : begin_char;
                    first_piece = false;
                }

                auto length = converter.measure(chunk.substr(chunk_offset, piece_size));
                emit_relative(delta_line, delta_start, length, token.kind, token.modifiers);

                chunk_offset += piece_size;
                piece_size = 0;
            }

            if(piece_size > 0) {
                auto length = converter.measure(chunk.substr(chunk_offset));
                emit_relative(1, 0, length, token.kind, token.modifiers);
            }
        }

        last_line = end_line;
        last_start_character = begin_char;
    }

private:
    void emit_relative(std::uint32_t delta_line,
                       std::uint32_t delta_start,
                       std::uint32_t token_length,
                       SymbolKind kind,
                       std::uint32_t modifiers) {
        if(token_length == 0) {
            return;
        }

        output.data.push_back(delta_line);
        output.data.push_back(delta_start);
        output.data.push_back(token_length);
        output.data.push_back(type_index(kind));
        output.data.push_back(encode_modifiers(modifiers));
    }

private:
    llvm::StringRef content;
    PositionMapper converter;
    protocol::SemanticTokens& output;
    std::uint32_t last_line = 0;
    std::uint32_t last_start_character = 0;
};

}  // namespace

auto semantic_tokens(CompilationUnitRef unit, PositionEncoding encoding)
    -> protocol::SemanticTokens {
    SemanticTokensCollector collector(unit);
    auto tokens = collector.collect();

    protocol::SemanticTokens result;
    result.data.reserve(tokens.size() * 5);

    SemanticTokenEncoder encoder(unit.interested_content(), encoding, result);
    for(const auto& token: tokens) {
        encoder.append(token);
    }

    return result;
}

}  // namespace clice::feature
