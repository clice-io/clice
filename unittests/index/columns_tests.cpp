module;

#include "support/prelude.h"

module clice;

import :index.columns;
import :unittests.utils;

namespace clice::testing {
namespace {

using index::read_bitmap;
using index::write_bitmap;

std::optional<Bitmap> view_of(const std::vector<std::byte>& image) {
    return read_bitmap(image.data(), image.size());
}

ZEST_SUITE(Columns) {

ZEST_CASE(ReadBitmapMatches) {  // Array containers alone (no run cookie, offsets stored), one run
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
        ZASSERT(view);
        ZEXPECT(*view == *bitmap);
        ZEXPECT(view->cardinality() == bitmap->cardinality());
        ZEXPECT(view->contains(bitmap->minimum()));
        ZEXPECT((*view & arrays) == (*bitmap & arrays));

        // A view moves with its arena; the moved-from shell frees nothing.
        Bitmap moved = std::move(*view);
        std::vector<Bitmap> held;
        held.push_back(std::move(moved));
        held.reserve(64);
        ZEXPECT(held.front() == *bitmap);
    }

    auto empty = write_bitmap(Bitmap{});
    ZASSERT(view_of(empty));
    ZEXPECT(view_of(empty)->isEmpty());
}

ZEST_CASE(ReadBitmapRejectsMalformed) {
    Bitmap bitmap;
    bitmap.add(1);
    bitmap.add(2);
    bitmap.add(3);
    // One array container: cookie, container count, key and cardinality,
    // the payload offset, then the three values.
    auto image = write_bitmap(bitmap);
    ZASSERT(image.size() == 22u);
    ZASSERT(view_of(image));

    ZEXPECT(!read_bitmap(image.data(), 0).has_value());
    auto truncated = image;
    truncated.pop_back();
    ZEXPECT(!view_of(truncated).has_value());

    // The bounded reader walks the payload and ignores the offset, so
    // the image stays valid to it; the in-place reader follows the offset.
    auto skewed = image;
    skewed[12] = std::byte{0xff};
    auto* walked = roaring::api::roaring_bitmap_portable_deserialize_safe(
        reinterpret_cast<const char*>(skewed.data()),
        skewed.size());
    ZEXPECT(walked != nullptr);
    roaring::api::roaring_bitmap_free(walked);
    ZEXPECT(!view_of(skewed).has_value());
    auto rewound = image;
    rewound[12] = std::byte{0};
    ZEXPECT(!view_of(rewound).has_value());

    // Values out of order fail the structural check.
    auto unsorted = image;
    std::swap(unsorted[16], unsorted[20]);
    ZEXPECT(!view_of(unsorted).has_value());
}

ZEST_CASE(VarintRoundTrip) {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint64_t> values = {0, 1, 127, 128, 300, ~std::uint64_t(0)};
    for(auto value: values) {
        index::write_varint(bytes, value);
    }
    std::uint32_t at = 0;
    for(auto value: values) {
        ZEXPECT(index::read_varint(bytes, at) == value);
    }
    ZEXPECT(at == bytes.size());
}

ZEST_CASE(VarintRejectsMalformed) {
    auto read = [](std::vector<std::uint8_t> bytes) {
        std::uint32_t at = 0;
        return index::read_varint(bytes, at);
    };
    // Cut short, a needless trailing zero byte, and a tenth byte past bit 63.
    ZEXPECT(!read({0x80}).has_value());
    ZEXPECT(!read({0x81, 0x00}).has_value());
    ZEXPECT(!read({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02}).has_value());
    ZEXPECT(read({0x00}) == std::uint64_t(0));
}

};  // ZEST_SUITE(Columns)

}  // namespace
}  // namespace clice::testing
