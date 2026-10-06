module;

#if defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)
#include "linux-x64/simdjson.fragment.h"
#elif defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)
#include "linux-x64-debug/simdjson.fragment.h"
#elif defined(__linux__) && defined(__aarch64__)
#include "linux-arm64/simdjson.fragment.h"
#elif defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)
#include "macos-arm64/simdjson.fragment.h"
#elif defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)
#include "macos-arm64-debug/simdjson.fragment.h"
#elif defined(__APPLE__) && defined(__x86_64__)
#include "macos-x64/simdjson.fragment.h"
#else
#error "no configuration merged matches this compilation"
#endif

export module simdjson;

export using ::operator ""_padded;
export using ::size_t;

export namespace simdjson {
using ::simdjson::BIGINT_ERROR;
using ::simdjson::CAPACITY;
using ::simdjson::DEPTH_ERROR;
using ::simdjson::EMPTY;
using ::simdjson::ErrorValues;
using ::simdjson::F_ATOM_ERROR;
using ::simdjson::INCOMPLETE_ARRAY_OR_OBJECT;
using ::simdjson::INCORRECT_TYPE;
using ::simdjson::INDEX_OUT_OF_BOUNDS;
using ::simdjson::INSUFFICIENT_PADDING;
using ::simdjson::INVALID_JSON_POINTER;
using ::simdjson::INVALID_URI_FRAGMENT;
using ::simdjson::IO_ERROR;
using ::simdjson::MEMALLOC;
using ::simdjson::NO_SUCH_FIELD;
using ::simdjson::NUMBER_ERROR;
using ::simdjson::NUMBER_OUT_OF_RANGE;
using ::simdjson::NUM_ERROR_CODES;
using ::simdjson::N_ATOM_ERROR;
using ::simdjson::OUT_OF_BOUNDS;
using ::simdjson::OUT_OF_CAPACITY;
using ::simdjson::OUT_OF_ORDER_ITERATION;
using ::simdjson::PARSER_IN_USE;
using ::simdjson::SCALAR_DOCUMENT_AS_VALUE;
using ::simdjson::SIMDJSON_VERSION_MAJOR;
using ::simdjson::SIMDJSON_VERSION_MINOR;
using ::simdjson::SIMDJSON_VERSION_REVISION;
using ::simdjson::STRING_ERROR;
using ::simdjson::SUCCESS;
using ::simdjson::TAPE_ERROR;
using ::simdjson::TRAILING_CONTENT;
using ::simdjson::T_ATOM_ERROR;
using ::simdjson::UNCLOSED_STRING;
using ::simdjson::UNESCAPED_CHARS;
using ::simdjson::UNEXPECTED_ERROR;
using ::simdjson::UNINITIALIZED;
using ::simdjson::UNSUPPORTED_ARCHITECTURE;
using ::simdjson::UTF8_ERROR;
using ::simdjson::builtin_implementation;
using ::simdjson::custom_deserializable;
using ::simdjson::deserializable;
using ::simdjson::deserialize;
using ::simdjson::deserialize_tag;
using ::simdjson::error_code;
using ::simdjson::error_message;
using ::simdjson::get_active_implementation;
using ::simdjson::get_available_implementations;
using ::simdjson::get_next_key_and_json_path;
using ::simdjson::has_custom_serialization;
using ::simdjson::implementation;
using ::simdjson::is_builtin_deserializable;
using ::simdjson::is_builtin_deserializable_v;
using ::simdjson::is_fatal;
using ::simdjson::is_streaming;
using ::simdjson::json_path_to_pointer_conversion;
using ::simdjson::minify;
using ::simdjson::nothrow_custom_deserializable;
using ::simdjson::nothrow_deserializable;
using ::simdjson::nothrow_tag_invocable;
using ::simdjson::operator<<;
using ::simdjson::pad;
using ::simdjson::pad_with_reserve;
using ::simdjson::padded_string;
using ::simdjson::padded_string_view;
using ::simdjson::prettify;
using ::simdjson::require_custom_serialization;
using ::simdjson::serialize;
using ::simdjson::serialize_tag;
using ::simdjson::simdjson_error;
using ::simdjson::simdjson_result;
using ::simdjson::stage1_mode;
using ::simdjson::tag_invocable;
using ::simdjson::tag_invoke;
using ::simdjson::to_json;
using ::simdjson::to_json_string;
using ::simdjson::to_string;
using ::simdjson::trim;
using ::simdjson::validate_utf8;
}

export namespace simdjson::concepts {
using ::simdjson::concepts::appendable_containers;
using ::simdjson::concepts::constructible_from_string_view;
using ::simdjson::concepts::container_but_not_string;
using ::simdjson::concepts::emplace_one;
using ::simdjson::concepts::indexable_container;
using ::simdjson::concepts::indexable_container_v;
using ::simdjson::concepts::is_pair;
using ::simdjson::concepts::optional_type;
using ::simdjson::concepts::returns_reference;
using ::simdjson::concepts::smart_pointer;
using ::simdjson::concepts::string_like;
using ::simdjson::concepts::string_view_keyed_map;
using ::simdjson::concepts::string_view_like;
}

export namespace simdjson::concepts::details {
using ::simdjson::concepts::details::supports_add;
using ::simdjson::concepts::details::supports_append;
using ::simdjson::concepts::details::supports_emplace;
using ::simdjson::concepts::details::supports_emplace_back;
using ::simdjson::concepts::details::supports_insert;
using ::simdjson::concepts::details::supports_op_append;
using ::simdjson::concepts::details::supports_push;
using ::simdjson::concepts::details::supports_push_back;
}

export namespace simdjson::constevalutil {
using ::simdjson::constevalutil::consteval_to_quoted_escaped;
using ::simdjson::constevalutil::fixed_string;
using ::simdjson::constevalutil::string_constant;
}

export namespace simdjson::convert::internal {
using ::simdjson::convert::internal::auto_parser;
using ::simdjson::convert::internal::to_adaptor;
}

export namespace simdjson::dom {
using ::simdjson::dom::array;
using ::simdjson::dom::document;
using ::simdjson::dom::document_stream;
using ::simdjson::dom::element;
using ::simdjson::dom::element_type;
using ::simdjson::dom::is_pointer_well_formed;
using ::simdjson::dom::key_value_pair;
using ::simdjson::dom::object;
using ::simdjson::dom::operator<<;
using ::simdjson::dom::parser;
}

export namespace simdjson::internal {
using ::simdjson::internal::ALTIVEC;
using ::simdjson::internal::AVX2;
using ::simdjson::internal::AVX512BW;
using ::simdjson::internal::AVX512CD;
using ::simdjson::internal::AVX512DQ;
using ::simdjson::internal::AVX512ER;
using ::simdjson::internal::AVX512F;
using ::simdjson::internal::AVX512IFMA;
using ::simdjson::internal::AVX512PF;
using ::simdjson::internal::AVX512VBMI2;
using ::simdjson::internal::AVX512VL;
using ::simdjson::internal::BMI1;
using ::simdjson::internal::BMI2;
using ::simdjson::internal::BitsSetTable256mul2;
using ::simdjson::internal::DEFAULT;
using ::simdjson::internal::LASX;
using ::simdjson::internal::LSX;
using ::simdjson::internal::NEON;
using ::simdjson::internal::PCLMULQDQ;
using ::simdjson::internal::SSE42;
using ::simdjson::internal::allocate_padded_buffer;
using ::simdjson::internal::atomic_ptr;
using ::simdjson::internal::available_implementation_list;
using ::simdjson::internal::base_formatter;
using ::simdjson::internal::digit_to_val32;
using ::simdjson::internal::dom_parser_implementation;
using ::simdjson::internal::error_code_info;
using ::simdjson::internal::error_codes;
using ::simdjson::internal::escape_json_string;
using ::simdjson::internal::from_chars;
using ::simdjson::internal::instruction_set;
using ::simdjson::internal::mini_formatter;
using ::simdjson::internal::operator<<;
using ::simdjson::internal::power_of_five_128;
using ::simdjson::internal::power_of_ten;
using ::simdjson::internal::pretty_formatter;
using ::simdjson::internal::pshufb_combine_table;
using ::simdjson::internal::simdjson_result_base;
using ::simdjson::internal::string_builder;
using ::simdjson::internal::structural_or_whitespace;
using ::simdjson::internal::structural_or_whitespace_negated;
using ::simdjson::internal::tape_ref;
using ::simdjson::internal::tape_type;
using ::simdjson::internal::thintable_epi8;
using ::simdjson::internal::to_chars;
using ::simdjson::internal::value128;
}

#if (defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)) || (defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)) || (defined(__linux__) && defined(__aarch64__)) || (defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(__APPLE__) && defined(__x86_64__))
export namespace simdjson::dom {
using ::simdjson::dom::stage1_worker;
}
#endif

#if (defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)) || (defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)) || (defined(__APPLE__) && defined(__x86_64__))
export namespace simdjson {
namespace builder = ::simdjson::fallback::builder;
namespace builtin = ::simdjson::fallback;
namespace ondemand = ::simdjson::fallback::ondemand;
}

export namespace simdjson::fallback {
using ::simdjson::fallback::dom_parser_implementation;
using ::simdjson::fallback::implementation;
using ::simdjson::fallback::implementation_simdjson_result_base;
using ::simdjson::fallback::number_type;
using ::simdjson::fallback::open_container;
using ::simdjson::fallback::operator<<;
}

export namespace simdjson::fallback::builder {
using ::simdjson::fallback::builder::escape_json_char;
using ::simdjson::fallback::builder::fast_needs_escaping;
using ::simdjson::fallback::builder::find_next_json_quotable_character;
using ::simdjson::fallback::builder::simple_needs_escaping;
using ::simdjson::fallback::builder::string_builder;
using ::simdjson::fallback::builder::write_string_escaped;
}

export namespace simdjson::fallback::builder::internal {
using ::simdjson::fallback::builder::internal::digit_count;
using ::simdjson::fallback::builder::internal::fast_digit_count_32;
using ::simdjson::fallback::builder::internal::fast_digit_count_64;
using ::simdjson::fallback::builder::internal::int_log2;
}

export namespace simdjson::fallback::numberparsing {
using ::simdjson::fallback::numberparsing::full_multiplication;
using ::simdjson::fallback::numberparsing::parse_number;
using ::simdjson::fallback::numberparsing::write_float;
}

export namespace simdjson::fallback::ondemand {
using ::simdjson::fallback::ondemand::array;
using ::simdjson::fallback::ondemand::array_iterator;
using ::simdjson::fallback::ondemand::depth_t;
using ::simdjson::fallback::ondemand::document;
using ::simdjson::fallback::ondemand::document_reference;
using ::simdjson::fallback::ondemand::document_stream;
using ::simdjson::fallback::ondemand::field;
using ::simdjson::fallback::ondemand::is_pointer_well_formed;
using ::simdjson::fallback::ondemand::json_iterator;
using ::simdjson::fallback::ondemand::json_type;
using ::simdjson::fallback::ondemand::number;
using ::simdjson::fallback::ondemand::number_type;
using ::simdjson::fallback::ondemand::object;
using ::simdjson::fallback::ondemand::object_iterator;
using ::simdjson::fallback::ondemand::operator!=;
using ::simdjson::fallback::ondemand::operator<<;
using ::simdjson::fallback::ondemand::operator==;
using ::simdjson::fallback::ondemand::parser;
using ::simdjson::fallback::ondemand::raw_json_string;
using ::simdjson::fallback::ondemand::release_parser;
using ::simdjson::fallback::ondemand::stage1_worker;
using ::simdjson::fallback::ondemand::token_iterator;
using ::simdjson::fallback::ondemand::token_position;
using ::simdjson::fallback::ondemand::value;
using ::simdjson::fallback::ondemand::value_iterator;
}

export namespace simdjson::fallback::ondemand::logger {
using ::simdjson::fallback::ondemand::logger::log_level;
}
#endif

#if (defined(__linux__) && defined(__aarch64__)) || (defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG))
export namespace simdjson {
namespace builder = ::simdjson::arm64::builder;
namespace builtin = ::simdjson::arm64;
namespace ondemand = ::simdjson::arm64::ondemand;
}

export namespace simdjson::arm64 {
using ::simdjson::arm64::dom_parser_implementation;
using ::simdjson::arm64::implementation;
using ::simdjson::arm64::implementation_simdjson_result_base;
using ::simdjson::arm64::number_type;
using ::simdjson::arm64::open_container;
using ::simdjson::arm64::operator<<;
}

export namespace simdjson::arm64::builder {
using ::simdjson::arm64::builder::escape_json_char;
using ::simdjson::arm64::builder::fast_needs_escaping;
using ::simdjson::arm64::builder::find_next_json_quotable_character;
using ::simdjson::arm64::builder::simple_needs_escaping;
using ::simdjson::arm64::builder::string_builder;
using ::simdjson::arm64::builder::write_string_escaped;
}

export namespace simdjson::arm64::builder::internal {
using ::simdjson::arm64::builder::internal::digit_count;
using ::simdjson::arm64::builder::internal::fast_digit_count_32;
using ::simdjson::arm64::builder::internal::fast_digit_count_64;
using ::simdjson::arm64::builder::internal::int_log2;
}

export namespace simdjson::arm64::numberparsing {
using ::simdjson::arm64::numberparsing::full_multiplication;
using ::simdjson::arm64::numberparsing::parse_number;
using ::simdjson::arm64::numberparsing::write_float;
}

export namespace simdjson::arm64::ondemand {
using ::simdjson::arm64::ondemand::array;
using ::simdjson::arm64::ondemand::array_iterator;
using ::simdjson::arm64::ondemand::depth_t;
using ::simdjson::arm64::ondemand::document;
using ::simdjson::arm64::ondemand::document_reference;
using ::simdjson::arm64::ondemand::document_stream;
using ::simdjson::arm64::ondemand::field;
using ::simdjson::arm64::ondemand::is_pointer_well_formed;
using ::simdjson::arm64::ondemand::json_iterator;
using ::simdjson::arm64::ondemand::json_type;
using ::simdjson::arm64::ondemand::number;
using ::simdjson::arm64::ondemand::number_type;
using ::simdjson::arm64::ondemand::object;
using ::simdjson::arm64::ondemand::object_iterator;
using ::simdjson::arm64::ondemand::operator!=;
using ::simdjson::arm64::ondemand::operator<<;
using ::simdjson::arm64::ondemand::operator==;
using ::simdjson::arm64::ondemand::parser;
using ::simdjson::arm64::ondemand::raw_json_string;
using ::simdjson::arm64::ondemand::release_parser;
using ::simdjson::arm64::ondemand::token_iterator;
using ::simdjson::arm64::ondemand::token_position;
using ::simdjson::arm64::ondemand::value;
using ::simdjson::arm64::ondemand::value_iterator;
}

export namespace simdjson::arm64::ondemand::logger {
using ::simdjson::arm64::ondemand::logger::log_level;
}
#endif

#if (defined(__linux__) && defined(__aarch64__)) || (defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG))
export namespace simdjson::arm64::ondemand {
using ::simdjson::arm64::ondemand::stage1_worker;
}
#endif
