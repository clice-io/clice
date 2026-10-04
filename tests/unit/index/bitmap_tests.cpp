#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "test/test.h"
#include "index/serialization.h"

namespace clice::testing {
namespace {

using index::read_bitmap;
using index::view_bitmap;
using index::write_bitmap;

std::optional<Bitmap> view_of(const std::vector<std::byte>& image) {
    return view_bitmap(image.data(), image.size());
}

std::optional<Bitmap> read_of(const std::vector<std::byte>& image) {
    return read_bitmap(image.data(), image.size());
}

ZEST_SUITE(BitmapImage) {

ZEST_CASE(ViewMatchesRead) {  // Array containers alone (no run cookie, offsets stored), one run
    // container (too few containers to store offsets), and bitset, run
    // and array containers together (offsets stored).
    Bitmap arrays;
    for(std::uint32_t i = 0; i < 300; i += 1) {
        arrays.add(i * 7);
    }
    Bitmap runs;
    runs.addRange(10, 5000);
    Bitmap mixed;
    for(std::uint32_t i = 0; i < 65536; i += 2) {
        mixed.add(i);
    }
    mixed.addRange(65536 + 100, 65536 + 60000);
    mixed.add(3 << 16);
    mixed.add(5 << 16);

    for(const auto* bitmap: {&arrays, &runs, &mixed}) {
        auto image = write_bitmap(*bitmap);
        auto view = view_of(image);
        auto read = read_of(image);
        ASSERT(view);
        ASSERT(read);
        EXPECT(*view == *read);
        EXPECT(view->cardinality() == bitmap->cardinality());
        EXPECT(view->contains(bitmap->minimum()));
        EXPECT((*view & arrays) == (*bitmap & arrays));

        // A view moves with its arena; the moved-from shell frees nothing.
        Bitmap moved = std::move(*view);
        std::vector<Bitmap> held;
        held.push_back(std::move(moved));
        held.reserve(64);
        EXPECT(held.front() == *bitmap);
    }

    auto empty = write_bitmap(Bitmap{});
    ASSERT(view_of(empty));
    EXPECT(view_of(empty)->isEmpty());
}

ZEST_CASE(ViewRejectsMalformed) {
    Bitmap bitmap;
    bitmap.add(1);
    bitmap.add(2);
    bitmap.add(3);
    // One array container: cookie, container count, key and cardinality,
    // the payload offset, then the three values.
    auto image = write_bitmap(bitmap);
    ASSERT(image.size() == 22u);
    ASSERT(view_of(image));

    EXPECT(!view_bitmap(image.data(), 0).has_value());
    auto truncated = image;
    truncated.pop_back();
    EXPECT(!view_of(truncated).has_value());

    // The bounded reader walks the payload and ignores the offset; the
    // in-place reader follows it.
    auto skewed = image;
    skewed[12] = std::byte{0xff};
    EXPECT(read_of(skewed));
    EXPECT(!view_of(skewed).has_value());
    auto rewound = image;
    rewound[12] = std::byte{0};
    EXPECT(!view_of(rewound).has_value());

    // Values out of order fail the structural check of both readers.
    auto unsorted = image;
    std::swap(unsorted[16], unsorted[20]);
    EXPECT(!read_of(unsorted).has_value());
    EXPECT(!view_of(unsorted).has_value());
}

};  // ZEST_SUITE(BitmapImage)

}  // namespace
}  // namespace clice::testing
