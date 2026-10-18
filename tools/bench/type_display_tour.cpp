// Everyday standard library code for tools/bench/type_display_check.ts:
// every declaration below deduces or names a type a reader sees in a hover
// card, an inlay hint, a completion or a signature.

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using namespace std::literals;

namespace app {

struct Point {
    int x;
    int y;

    int norm() const {
        return x * x + y * y;
    }
};

using Id = std::uint32_t;
using UserId = Id;
using Names = std::vector<std::string>;
using Index = std::map<std::string, std::vector<Point>>;

UserId current_user();
Names load_names();
Index load_index();

struct Graph {
    using Node = int;
    using Edges = std::vector<Node>;

    Edges edges(Node node) const;
    std::size_t size() const;
};

void containers(std::vector<int>& v,
                std::map<std::string, Point>& m,
                std::unordered_map<std::string, int>& counts,
                const std::set<long>& ordered,
                std::deque<double>& queue,
                std::list<Point>& points,
                std::array<int, 4>& fixed) {
    auto it = v.begin();
    auto end = v.cend();
    auto count = v.size();
    auto& first = v.front();
    auto last = v.back();
    auto at = v.at(0);
    auto data = v.data();
    auto distance = end - it;
    auto reversed = v.rbegin();
    auto capacity = v.capacity();

    auto found = m.find("key");
    auto& entry = *m.begin();
    auto [where, inserted] = m.insert({
        "k",
        Point{1, 2}
    });
    auto [key, value] = *m.begin();
    auto& [key_ref, value_ref] = *found;
    for(auto& [name, point]: m) {
        auto norm = point.norm();
        auto length = name.size();
    }
    auto range = m.equal_range("a");
    auto node = m.extract("b");
    auto lower = m.lower_bound("c");
    auto mapped = m["d"];

    auto bucket = counts.begin();
    auto hit = counts.find("x");
    auto [hit_key, hit_count] = *hit;
    auto load = counts.load_factor();
    auto hasher = counts.hash_function();

    auto smallest = ordered.begin();
    auto contains = ordered.contains(3);
    auto front = queue.front();
    auto queue_it = queue.begin();
    auto point_it = points.begin();
    auto& point_ref = points.back();
    auto fixed_it = fixed.begin();
    auto fixed_size = fixed.size();
    auto fixed_front = fixed.front();
}

void strings(const std::string& text, std::string_view view, std::wstring& wide) {
    auto copy = text;
    auto length = text.size();
    auto position = text.find('x');
    auto sub = text.substr(1);
    auto c = text[0];
    auto c_str = text.c_str();
    auto text_it = text.begin();
    auto view_size = view.size();
    auto view_sub = view.substr(1);
    auto view_it = view.begin();
    auto literal = "abc";
    auto string_literal = "abc"s;
    auto view_literal = "abc"sv;
    auto wide_size = wide.size();
    auto formatted = std::format("{} {}", 1, text);
    auto number = std::to_string(42);
    auto parsed = std::stoi(text);
    std::istringstream stream(text);
    auto stream_size = stream.gcount();
    auto pmr = std::pmr::string("x");
    auto pmr_size = pmr.size();
    std::regex pattern("a+");
    std::smatch match;
    auto matched = std::regex_search(text, match, pattern);
    auto group = match[0];
    auto group_text = match.str(0);
}

void ranges(std::vector<int>& v, const std::vector<std::string>& names) {
    auto evens = v | std::views::filter([](int x) { return x % 2 == 0; });
    auto doubled = evens | std::views::transform([](int x) { return x * 2; });
    auto first_even = evens.begin();
    auto numbers = std::views::iota(0, 10);
    auto taken = numbers | std::views::take(3);
    auto reversed = std::views::reverse(v);
    auto keys = std::views::keys(std::map<int, int>{});
    for(auto x: doubled) {
        auto y = x + 1;
    }
    auto found = std::ranges::find(v, 3);
    auto [min, max] = std::ranges::minmax(v);
    auto sorted = std::ranges::sort(v);
    auto sum = std::accumulate(v.begin(), v.end(), 0);
    auto total = std::accumulate(v.begin(), v.end(), 0L);
    auto biggest = std::max_element(v.begin(), v.end());
    auto counted = std::count(v.begin(), v.end(), 1);
    auto sub = std::ranges::subrange(v.begin(), v.end());
    auto split = names | std::views::join;
#ifdef __cpp_lib_ranges_zip
    auto zipped = std::views::zip(v, names);
    for(auto [number, name]: zipped) {
        auto n = number;
    }
#endif
#ifdef __cpp_lib_ranges_chunk
    auto chunks = v | std::views::chunk(2);
    auto adjacent = v | std::views::adjacent<2>;
#endif
    auto inserter = std::back_inserter(v);
    auto next = std::next(v.begin());
    auto moved = std::make_move_iterator(v.begin());
}

void callables(std::vector<int>& v) {
    auto twice = [](int x) {
        return x * 2;
    };
    auto add = [&](auto y) {
        return y + v.size();
    };
    auto measure = [](const std::string& s) -> std::size_t {
        return s.size();
    };
    auto compare = [](const Point& a, const Point& b) {
        return a.x < b.x;
    };
    auto result = twice(2);
    auto added = add(1);
    std::function<int(int)> fn = twice;
    auto called = fn(1);
    auto bound = std::bind_front(twice, 1);
    auto bind_result = bound();
    auto member = std::mem_fn(&Point::norm);
    auto norm = member(Point{1, 2});
    std::set<Point, decltype(compare)> by_x(compare);
    auto by_x_it = by_x.begin();
    auto ref = std::ref(v);
    auto& unwrapped = ref.get();
    auto invoked = std::invoke(twice, 3);
    auto pointer = &containers;
    auto method = &Point::norm;
    auto field = &Point::x;
    auto recursive = [](this auto self, int n) -> int {
        return n <= 1 ? 1 : n * self(n - 1);
    };
    auto generic = []<typename T>(T value) {
        return std::vector<T>{value};
    };
    auto made = generic(1.0);
}

void pointers() {
    auto unique = std::make_unique<Point>();
    auto shared = std::make_shared<Point>(1, 2);
    auto array = std::make_unique<int[]>(3);
    auto& pointee = *unique;
    auto raw = unique.get();
    auto weak = std::weak_ptr(shared);
    auto locked = weak.lock();
    auto use_count = shared.use_count();
    auto element = array[0];
    auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(std::fopen("x", "r"), &std::fclose);
    auto handle = file.get();
    auto deleter = file.get_deleter();
    auto casted = std::static_pointer_cast<const Point>(shared);
    auto owner = shared.owner_before(weak);
}

void time() {
    auto start = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    auto ticks = millis.count();
    auto seconds = 5s;
    auto timeout = 100ms;
    auto sum = seconds + timeout;
    auto wall = std::chrono::system_clock::now();
    auto day = std::chrono::floor<std::chrono::days>(wall);
    auto date = std::chrono::year_month_day(day);
    auto year = date.year();
    auto epoch = wall.time_since_epoch();
    auto as_time = std::chrono::system_clock::to_time_t(wall);
    auto hours = std::chrono::hours(3);
    auto mixed = hours + 30min;
}

void vocabulary(std::optional<Point>& maybe,
                std::variant<int, std::string>& either,
                std::tuple<int, double, std::string>& triple,
                std::expected<int, std::string>& outcome) {
    auto value = maybe.value();
    auto& point = *maybe;
    auto fallback = maybe.value_or(Point{});
    auto mapped = maybe.transform([](const Point& p) { return p.x; });
    auto index = either.index();
    auto& text = std::get<std::string>(either);
    auto alternative = std::get_if<int>(&either);
    auto visited = std::visit([](auto& x) { return sizeof(x); }, either);
    auto [i, d, s] = triple;
    auto& second = std::get<1>(triple);
    auto made = std::make_tuple(1, "two", 3.0);
    auto tied = std::tie(i, d);
    auto pair = std::make_pair(1, std::string("x"));
    auto [left, right] = pair;
    auto ctad = std::pair{1, 2.0};
    auto succeeded = outcome.value();
    auto error = outcome.error();
    std::any any = 1;
    auto cast = std::any_cast<int>(any);
    auto bits = std::bitset<8>(5);
    auto bit = bits[0];
    std::vector<bool> flags(4);
    auto flag = flags[0];
    auto flag_it = flags.begin();
    auto list = {1, 2, 3};
    auto deduced = std::array{1, 2, 3};
    auto vector = std::vector{1, 2};
    auto span = std::span(vector);
    auto span_size = span.size();
    auto byte = std::byte{1};
}

void system(std::mutex& lock, std::atomic<int>& counter, const std::filesystem::path& file) {
    auto guard = std::lock_guard(lock);
    auto unique = std::unique_lock(lock);
    auto loaded = counter.load();
    auto previous = counter.fetch_add(1);
    auto extension = file.extension();
    auto native = file.native();
    auto text = file.string();
    auto parent = file.parent_path();
    auto entries = std::filesystem::directory_iterator(file);
    for(auto& entry: entries) {
        auto size = entry.file_size();
        auto modified = entry.last_write_time();
    }
    std::mt19937 engine;
    auto dist = std::uniform_int_distribution(1, 6);
    auto roll = dist(engine);
    auto random = engine();
    auto size = sizeof(int);
    auto difference = &counter - &counter;
    auto big = 1ull << 40;
    std::uint64_t wide = 1;
    auto widened = wide * 2;
    std::int32_t small = 1;
    auto promoted = small + 1;
    auto alignment = alignof(Point);
}

void user_types(Graph& graph, Index& index) {
    auto user = current_user();
    auto names = load_names();
    auto first_name = names.front();
    auto loaded = load_index();
    auto edges = graph.edges(0);
    auto edge = edges.front();
    auto edge_it = edges.begin();
    auto graph_size = graph.size();
    auto bucket = index.find("x");
    auto& points = bucket->second;
    auto& [bucket_key, bucket_points] = *bucket;
    auto point = points.front();
    int array[3] = {};
    auto& array_ref = array;
    auto array_pointer = &array;
    auto decayed = array;
    auto heap = new int[3];
    decltype(auto) element = array[0];
    decltype(names) copy = names;
    decltype(names)::value_type first = names[0];
}

template <typename T>
void generic(std::vector<T>& values, std::map<std::string, T>& by_name, T& item) {
    auto it = values.begin();
    auto count = values.size();
    auto& first = values.front();
    auto found = by_name.find("x");
    auto& [key, value] = *found;
    auto copy = item;
    auto pair = std::make_pair(item, count);
    auto view = values | std::views::take(2);
    typename std::vector<T>::iterator typed = values.begin();
    auto lambda = [&](const T& x) {
        return x;
    };
    values.push_back(item);
    by_name.emplace("y", item);
}

template <typename Container>
auto keys(const Container& container) {
    std::vector<typename Container::key_type> result;
    for(const auto& [key, value]: container) {
        result.push_back(key);
    }
    return result;
}

void instantiate(std::vector<std::string>& values,
                 std::map<std::string, std::string>& by_name,
                 std::string& item) {
    generic(values, by_name, item);
    auto names = keys(by_name);
    auto& name = names.front();
    auto it = values.begin();
    auto& front = values.front();
    auto value = values[0];
    values.emplace_back("x");
    by_name.insert_or_assign("x", item);
    std::ranges::sort(values);
    auto joined = std::accumulate(values.begin(), values.end(), std::string());
}

}  // namespace app
