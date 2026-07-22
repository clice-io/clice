module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <compare>
#include <concepts>
#include <coroutine>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <print>
#include <random>
#include <ranges>
#include <regex>
#include <set>
#include <source_location>
#include <span>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module stdlib;

// clang20 + libstdc++ befriend floor — the authoritative note.
//
// A `using ::std::NAME;` re-export is permanently impossible for a handful of
// std names: libstdc++ declares them as friends of a class template (e.g.
// std::shared_ptr befriends std::make_shared, std::ostreambuf_iterator
// befriends std::copy), and a using-declaration cannot be the target of a
// friend declaration — clang errors "cannot befriend target of using
// declaration" wherever that template gets instantiated. The instantiation can
// arrive either through a textual include OR through a module BMI that carried
// it, so the clash cannot be avoided at this wrapper. These names are therefore
// NOT re-exported; call sites include the specific declaring std header in
// their own global module fragment instead (<memory> for make_shared,
// <algorithm> for copy, <system_error>/<variant>/<vector>/<optional>/<memory>
// for the befriended free comparison operators). Do NOT add them to the
// using-lists below — the build will break.
//
// Affected names: make_shared, copy, the free comparison operators
// (operator== / operator<=> over std::optional, std::variant, std::vector,
// std::string, std::error_code, reverse_iterator, and std::bitset's &/|/^),
// and std::get (befriended by the MSVC STL's std::tuple; libstdc++/libc++ do
// not befriend it, so this one only clashes on Windows — the floor is
// uniform across platforms anyway).
export namespace std {

using ::std::abort;
using ::std::accumulate;
using ::std::array;
using ::std::atomic;
using ::std::atomic_bool;
using ::std::back_inserter;
using ::std::basic_string;
using ::std::bit_cast;
using ::std::bitset;
using ::std::byte;
using ::std::clamp;
using ::std::convertible_to;
// std::copy omitted — see the befriend-floor note at the top of this module.
using ::std::cout;
using ::std::deque;
using ::std::destroy_at;
using ::std::distance;
using ::std::erase;
using ::std::erase_if;
using ::std::errc;
using ::std::error_code;
using ::std::exchange;
using ::std::expected;
using ::std::find;
using ::std::find_if;
using ::std::format;
using ::std::format_string;
using ::std::formatter;
using ::std::forward;
using ::std::function;
using ::std::generic_category;
using ::std::get_if;
#ifndef _MSVC_STL_VERSION
// std::get: free non-friend on libstdc++/libc++ (pair/tuple structured
// bindings need the export); befriended by MSVC STL's tuple (export would
// clash), where friend-ADL finds it instead.
using ::std::get;
#endif
using ::std::getenv;
using ::std::greater;
using ::std::hash;
using ::std::initializer_list;
using ::std::int64_t;
using ::std::integral_constant;
using ::std::invoke_result_t;
using ::std::is_same_v;
using ::std::is_trivially_copyable_v;
using ::std::is_trivially_destructible_v;
using ::std::isalnum;
using ::std::list;
using ::std::lock_guard;
using ::std::make_error_code;
using ::std::make_move_iterator;
using ::std::make_pair;
// std::make_shared omitted — see the befriend-floor note at the top of this module.
using ::std::make_unique;
using ::std::map;
using ::std::max;
using ::std::memcpy;
using ::std::memory_order_relaxed;
using ::std::min;
using ::std::move;
using ::std::mt19937;
using ::std::mutex;
using ::std::nullopt;
using ::std::nullptr_t;
using ::std::numeric_limits;
using ::std::ofstream;
using ::std::operator+;
using ::std::optional;
using ::std::ostringstream;
using ::std::pair;
using ::std::partition_point;
using ::std::print;
using ::std::println;
using ::std::remove;
using ::std::remove_cvref_t;
using ::std::replace;
using ::std::reverse;
using ::std::same_as;
using ::std::set;
using ::std::shared_ptr;
using ::std::signal;
using ::std::size_t;
using ::std::source_location;
using ::std::span;
using ::std::stable_partition;
using ::std::stack;
using ::std::string;
using ::std::string_view;
using ::std::strong_ordering;
using ::std::swap;
using ::std::system_category;
using ::std::thread;
using ::std::tie;
using ::std::to_string;
using ::std::tolower;
using ::std::tuple;
using ::std::tuple_element;
using ::std::tuple_size;
using ::std::type_identity_t;
using ::std::uint16_t;
using ::std::uint32_t;
using ::std::uint64_t;
using ::std::uint8_t;
using ::std::underlying_type_t;
using ::std::unexpected;
using ::std::unique_ptr;
using ::std::unordered_map;
using ::std::unreachable;
using ::std::vector;
using ::std::visit;
using ::std::weak_ptr;

}  // namespace std

// C++20 coroutine support. Module units that define coroutines over kota's
// async types (task/event_loop, from the kota wrapper) need
// std::coroutine_traits and std::coroutine_handle reachable by qualified name
// for the co_await lowering; otherwise the <coroutine> vocabulary is only
// pulled in textually inside the wrapper GMFs and stays invisible here.
export namespace std {

using ::std::coroutine_handle;
using ::std::coroutine_traits;
using ::std::noop_coroutine;
using ::std::suspend_always;
using ::std::suspend_never;

}  // namespace std

export namespace std::chrono {

using ::std::chrono::duration_cast;
using ::std::chrono::hours;
using ::std::chrono::microseconds;
using ::std::chrono::milliseconds;
using ::std::chrono::minutes;
using ::std::chrono::nanoseconds;
using ::std::chrono::operator*;
using ::std::chrono::operator+;
using ::std::chrono::operator-;
using ::std::chrono::operator==;
using ::std::chrono::operator<=>;
using ::std::chrono::seconds;
using ::std::chrono::steady_clock;
using ::std::chrono::system_clock;

}  // namespace std::chrono

export namespace std::ranges {

using ::std::ranges::all_of;
using ::std::ranges::any_of;
using ::std::ranges::contains;
using ::std::ranges::copy;
using ::std::ranges::count;
using ::std::ranges::count_if;
using ::std::ranges::equal;
using ::std::ranges::equal_range;
using ::std::ranges::find;
using ::std::ranges::find_if;
using ::std::ranges::for_each;
using ::std::ranges::input_range;
using ::std::ranges::lower_bound;
using ::std::ranges::move;
using ::std::ranges::none_of;
using ::std::ranges::range_value_t;
using ::std::ranges::remove;
using ::std::ranges::replace;
using ::std::ranges::reverse;
using ::std::ranges::size;
using ::std::ranges::sort;
using ::std::ranges::to;
using ::std::ranges::transform;
using ::std::ranges::unique;

}  // namespace std::ranges

export namespace std {
namespace views = ::std::ranges::views;

}

export namespace std::ranges::views {

using ::std::ranges::views::zip;

}  // namespace std::ranges::views

export namespace std::inline literals {

using ::std::literals::string_literals::operator""s;
using ::std::literals::string_view_literals::operator""sv;

}  // namespace std::inline literals

// libstdc++ defines the container-iterator comparison/difference operators as
// free functions in __gnu_cxx (not hidden friends), so module ADL cannot find
// them; re-export the C++20 primitives used by range-for and iterator
// arithmetic over std::vector/std::string in module units. libc++ and the
// MSVC STL have no __gnu_cxx and structure these operators differently, so
// the block is libstdc++-only.
#ifdef __GLIBCXX__
export namespace __gnu_cxx {

using ::__gnu_cxx::operator-;
using ::__gnu_cxx::operator==;
using ::__gnu_cxx::operator<=>;

}  // namespace __gnu_cxx
#elif defined(_LIBCPP_VERSION)
// libc++ declares the same iterator operators as free function templates in
// namespace std over __wrap_iter (not hidden friends), so they need the same
// re-export. Safe here, unlike on libstdc++: libc++ does not befriend these
// operator names from its class templates.
export namespace std {

using ::std::operator-;
using ::std::operator==;
using ::std::operator<=>;

}  // namespace std
#endif

// std.compat-style: C library names at global scope.
export using ::abort;
export using ::exit;
export using ::getenv;
export using ::int64_t;
export using ::isalnum;
export using ::memcpy;
export using ::size_t;
#ifndef _WIN32
export using ::ssize_t;  // POSIX-only; Windows has no global ssize_t
#endif
export using ::system;
export using ::tolower;
export using ::uint16_t;
export using ::uint32_t;
export using ::uint64_t;
export using ::uint8_t;
export using ::uintptr_t;
