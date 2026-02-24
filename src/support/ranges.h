#pragma once

#include <concepts>
#include <ranges>

namespace clice {

namespace ranges = std::ranges;
namespace views = std::views;

enum class RangeKind {
    Map = 0,
    Set,
    Sequence,
    Invalid,
};

template <typename Range>
constexpr inline RangeKind range_kind = [] {
    if constexpr(std::same_as<Range, std::remove_cvref_t<ranges::range_reference_t<Range>>>) {
        return RangeKind::Invalid;
    } else if constexpr(requires { typename Range::key_type; }) {
        if constexpr(requires { typename Range::mapped_type; }) {
            return RangeKind::Map;
        } else {
            return RangeKind::Set;
        }
    } else {
        return RangeKind::Sequence;
    }
}();

template <typename Range>
concept map_range = ranges::input_range<Range> && range_kind<Range> == RangeKind::Map;

template <typename Range>
concept set_range = ranges::input_range<Range> && range_kind<Range> == RangeKind::Set;

template <typename Range>
concept sequence_range = ranges::input_range<Range> && range_kind<Range> == RangeKind::Sequence;

}  // namespace clice
