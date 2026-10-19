module;

#include "modules/prelude.h"

module clice;

import :support.archive;
import :unittests.utils;

namespace clice::testing {
namespace {

enum class Color : std::uint8_t {
    Red,
    Green,
};

/// A plain record with padding after `flag`.
struct Point {
    std::uint32_t x = 0;
    bool flag = false;
    std::uint64_t y = 0;

    friend bool operator==(const Point&, const Point&) = default;
};

template <template <typename> typename Seq>
struct LeafOf {
    Seq<char> name;
    Seq<std::uint16_t> values;

    friend bool operator==(const LeafOf&, const LeafOf&) = default;
};

using Leaf = LeafOf<archive::Owned>;

template <template <typename> typename Seq>
struct InnerOf {
    std::uint32_t id = 0;
    Seq<char> label;

    friend bool operator==(const InnerOf&, const InnerOf&) = default;
};

using Inner = InnerOf<archive::Owned>;

template <template <typename> typename Seq>
struct SampleOf {
    bool enabled = false;
    std::uint8_t small = 0;
    std::int32_t negative = 0;
    std::uint64_t wide = 0;
    float ratio = 0;
    double precise = 0;
    Color color = Color::Red;
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    Seq<char> text;
    Seq<char> empty_text;
    Seq<std::uint32_t> numbers;
    Seq<std::uint64_t> empty_numbers;
    Seq<Seq<char>> names;
    Seq<Point> points;
    Seq<LeafOf<Seq>> leaves;
    InnerOf<Seq> inner;
    Seq<std::uint8_t> bytes;

    friend bool operator==(const SampleOf&, const SampleOf&) = default;
};

using Sample = SampleOf<archive::Owned>;

static_assert(std::is_same_v<archive::Archived<Sample>, SampleOf<archive::Vec>>);
static_assert(std::is_same_v<archive::Archived<Point>, Point>);
static_assert(!std::is_copy_constructible_v<archive::Vec<std::uint32_t>>);
static_assert(sizeof(archive::Vec<std::uint64_t>) == 8);

Sample sample() {
    return {
        .enabled = true,
        .small = 7,
        .negative = -5,
        .wide = 1ull << 40,
        .ratio = 0.5f,
        .precise = 2.25,
        .color = Color::Green,
        .first = 11,
        .second = 22,
        .text = "hello",
        .numbers = {1, 2, 3},
        .names = {"a", "", "ccc"},
        .points = {{1, true, 2}, {3, false, 4}},
        .leaves = {{"x", {1, 2}}, {"", {}}, {"zz", {9}}},
        .inner = {42, "inner"},
        .bytes = {1, 2, 3, 4, 5},
    };
}

template <template <typename> typename Seq>
struct ProbeOf {
    bool flag = false;
    Seq<char> text;
    Seq<std::uint32_t> numbers;
};

using Probe = ProbeOf<archive::Owned>;

/// Bytes the archive is read from, at the alignment it asks for.
struct Aligned {
    std::vector<std::uint64_t> words;

    std::uint32_t size = 0;

    explicit Aligned(llvm::StringRef bytes) :
        words((bytes.size() + 7) / 8 + 1), size(static_cast<std::uint32_t>(bytes.size())) {
        std::memcpy(words.data(), bytes.data(), bytes.size());
    }

    llvm::StringRef bytes() const {
        return {reinterpret_cast<const char*>(words.data()), size};
    }

    char* data() {
        return reinterpret_cast<char*>(words.data());
    }
};

ZEST_SUITE(Archive) {

ZEST_CASE(RoundTrip) {
    auto value = sample();
    Aligned buffer(archive::encode(value));
    auto verified = archive::verify<Sample>(buffer.bytes());
    ZASSERT(verified);
    auto& root = **verified;
    ZEXPECT(root.enabled);
    ZEXPECT(root.small == 7);
    ZEXPECT(root.negative == -5);
    ZEXPECT(root.wide == (1ull << 40));
    ZEXPECT(root.ratio == 0.5f);
    ZEXPECT(root.precise == 2.25);
    ZEXPECT(root.color == Color::Green);
    ZEXPECT(root.first == 11u);
    ZEXPECT(root.second == 22u);
    ZEXPECT(llvm::StringRef(root.text) == "hello");
    ZEXPECT(root.empty_text.empty());
    ZEXPECT(llvm::ArrayRef<std::uint32_t>(root.numbers) ==
            llvm::ArrayRef<std::uint32_t>({1, 2, 3}));
    ZEXPECT(root.empty_numbers.empty());

    auto& names = root.names;
    ZASSERT(names.size() == 3u);
    ZEXPECT(names[0].str() == "a");
    ZEXPECT(names[1].empty());
    ZEXPECT(names[2].str() == "ccc");

    ZEXPECT(llvm::ArrayRef<Point>(root.points) == llvm::ArrayRef(value.points));

    auto& leaves = root.leaves;
    ZASSERT(leaves.size() == 3u);
    ZEXPECT(leaves[0].name.str() == "x");
    ZEXPECT(llvm::ArrayRef<std::uint16_t>(leaves[0].values) ==
            llvm::ArrayRef<std::uint16_t>({1, 2}));
    ZEXPECT(leaves[1].values.empty());
    ZEXPECT(leaves[2].name.str() == "zz");

    ZEXPECT(root.inner.id == 42u);
    ZEXPECT(root.inner.label.str() == "inner");

    Sample decoded;
    archive::decode(root, decoded);
    ZEXPECT(decoded == value);
}

ZEST_CASE(ReadInPlace) {
    // Sequences of plain elements are the archive's own bytes, at the
    // alignment every sequence starts at.
    Aligned buffer(archive::encode(sample()));
    auto verified = archive::verify<Sample>(buffer.bytes());
    ZASSERT(verified);
    auto& root = **verified;
    auto bytes = buffer.bytes();
    for(const void* data: {static_cast<const void*>(root.numbers.data()),
                           static_cast<const void*>(root.points.data()),
                           static_cast<const void*>(root.bytes.data())}) {
        auto at = static_cast<const char*>(data);
        ZEXPECT(at >= bytes.begin());
        ZEXPECT(at < bytes.end());
        ZEXPECT(reinterpret_cast<std::uintptr_t>(at) % archive::alignment == 0u);
    }
}

ZEST_CASE(NestedArchiveInPlace) {
    // An archive held as bytes of another one reads in place.
    auto inner = archive::encode(Inner{7, "seven"});
    Sample outer;
    outer.bytes.assign(inner.begin(), inner.end());
    Aligned buffer(archive::encode(outer));
    auto verified = archive::verify<Sample>(buffer.bytes());
    ZASSERT(verified);
    auto& held = (*verified)->bytes;
    auto nested = archive::verify<Inner>(
        llvm::StringRef(reinterpret_cast<const char*>(held.data()), held.size()));
    ZASSERT(nested);
    ZEXPECT((*nested)->label.str() == "seven");
}

ZEST_CASE(Canonical) {
    // Padding is written as zeros whatever the value's own bytes hold, so
    // equal values have equal archives.
    std::vector<Point> dirty(2);
    std::memset(static_cast<void*>(dirty.data()), 0xab, sizeof(Point) * dirty.size());
    dirty[0].x = 1;
    dirty[0].flag = true;
    dirty[0].y = 2;
    dirty[1].x = 3;
    dirty[1].flag = false;
    dirty[1].y = 4;
    auto value = sample();
    auto clean = archive::encode(value);
    value.points = dirty;
    ZEXPECT(archive::encode(value) == clean);
}

ZEST_CASE(RejectsOtherBytes) {
    auto bytes = archive::encode(sample());
    Aligned buffer(bytes);
    ZASSERT(archive::verify<Sample>(buffer.bytes()));

    // Another type's archive, and bytes that are no archive.
    Aligned other(archive::encode(Inner{1, "x"}));
    ZEXPECT(!archive::verify<Sample>(other.bytes()));
    Aligned junk(std::string(bytes.size(), 'x'));
    ZEXPECT(!archive::verify<Sample>(junk.bytes()));

    // Every cut, and one byte past the end.
    for(std::uint32_t size = 0; size < bytes.size(); size += 1) {
        ZEXPECT(!archive::verify<Sample>(buffer.bytes().take_front(size)));
    }
    Aligned longer(bytes + '\0');
    ZEXPECT(!archive::verify<Sample>(longer.bytes()));

    // Bytes off the alignment an archive is read at.
    Aligned shifted(" " + bytes);
    ZEXPECT(!archive::verify<Sample>(shifted.bytes().drop_front(1)));
}

ZEST_CASE(RejectsMisplacedParts) {
    // A pair pointing anywhere but where the writer puts its part, a
    // length past the bytes, an empty part with an offset, and a bool
    // other than 0 or 1 are all rejected; each mutation leaves the bytes'
    // size alone.
    Probe probe{
        .flag = true,
        .text = "abc",
        .numbers = {1, 2}
    };
    auto bytes = archive::encode(probe);
    // The header, then the root: the bool at 8, the string's pair at 12,
    // the sequence's at 20.
    constexpr std::uint32_t flag = 8;
    constexpr std::uint32_t text = 12;
    constexpr std::uint32_t numbers = 20;
    auto verifies = [&](auto mutate) {
        Aligned buffer(bytes);
        mutate(buffer.data());
        return archive::verify<Probe>(buffer.bytes()).has_value();
    };
    auto set_u32 = [](char* data, std::uint32_t at, std::uint32_t value) {
        std::memcpy(data + at, &value, sizeof(value));
    };
    ZASSERT(verifies([](char*) {}));
    ZEXPECT(!verifies([&](char* data) { data[flag] = 2; }));
    ZEXPECT(!verifies([&](char* data) { set_u32(data, text, 1); }));
    ZEXPECT(!verifies([&](char* data) { set_u32(data, text + 4, 6); }));
    ZEXPECT(!verifies([&](char* data) { set_u32(data, numbers + 4, 1u << 30); }));
    ZEXPECT(!verifies([&](char* data) {
        std::uint32_t pair[] = {8, 0};
        std::memcpy(data + numbers, pair, sizeof(pair));
    }));
}

ZEST_CASE(MeasuresFields) {
    std::vector<std::pair<std::string, std::uint64_t>> parts;
    Aligned buffer(archive::encode(sample()));
    auto verified = archive::verify<Sample>(buffer.bytes());
    ZASSERT(verified);
    archive::measure(**verified, [&](llvm::StringRef path, std::uint64_t bytes) {
        if(!parts.empty() && parts.back().first == path) {
            parts.back().second += bytes;
        } else {
            parts.emplace_back(path.str(), bytes);
        }
    });
    std::vector<std::pair<std::string, std::uint64_t>> expected = {
        {"text",          5        },
        {"empty_text",    0        },
        {"numbers",       12       },
        {"empty_numbers", 0        },
        {"names",         3 * 8 + 4},
        {"points",        2 * 16   },
        {"leaves",        3 * 16   },
        {"leaves.name",   1        },
        {"leaves.values", 4        },
        {"leaves.name",   0        },
        {"leaves.values", 0        },
        {"leaves.name",   2        },
        {"leaves.values", 2        },
        {"inner.label",   5        },
        {"bytes",         5        },
    };
    ZEXPECT(parts == expected);
}

};  // ZEST_SUITE(Archive)

}  // namespace
}  // namespace clice::testing
