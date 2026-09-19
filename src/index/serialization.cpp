#include "index/serialization.h"

#include <cstring>

namespace clice::index {

namespace {

std::uint16_t read16(const std::uint8_t* at) {
    std::uint16_t value;
    std::memcpy(&value, at, sizeof value);
    return value;
}

std::uint32_t read32(const std::uint8_t* at) {
    std::uint32_t value;
    std::memcpy(&value, at, sizeof value);
    return value;
}

/// Whether the container offsets an image's header carries name the
/// positions the containers occupy laid out back to back — the only
/// layout the serializer writes. Runs over an image the bounded size walk
/// accepted whole, so every read stays inside it.
bool offsets_canonical(const std::uint8_t* image) {
    using namespace roaring::internal;
    auto cookie = read32(image);
    bool has_runs = (cookie & 0xffff) == SERIAL_COOKIE;
    const std::uint8_t* at = image + sizeof cookie;
    std::uint32_t count;
    const std::uint8_t* run_flags = nullptr;
    if(has_runs) {
        count = (cookie >> 16) + 1;
        run_flags = at;
        at += (count + 7) / 8;
    } else {
        count = read32(at);
        at += sizeof count;
    }
    const std::uint8_t* headers = at;
    at += count * 2 * sizeof(std::uint16_t);
    if(has_runs && count < NO_OFFSET_THRESHOLD) {
        return true;
    }
    const std::uint8_t* offsets = at;
    at += count * sizeof(std::uint32_t);
    for(std::uint32_t i = 0; i < count; i += 1) {
        if(read32(offsets + i * sizeof(std::uint32_t)) != static_cast<std::size_t>(at - image)) {
            return false;
        }
        std::uint32_t cardinality = read16(headers + i * 2 * sizeof(std::uint16_t) + 2) + 1u;
        if(has_runs && (run_flags[i / 8] & (1 << (i % 8))) != 0) {
            at += sizeof(std::uint16_t) + read16(at) * sizeof(rle16_t);
        } else if(cardinality > DEFAULT_MAX_SIZE) {
            at += BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(std::uint64_t);
        } else {
            at += cardinality * sizeof(std::uint16_t);
        }
    }
    return true;
}

}  // namespace

std::optional<Bitmap> view_bitmap(const void* data, std::size_t size) {
    const auto* image = static_cast<const char*>(data);
    if(size == 0 || roaring::api::roaring_bitmap_portable_deserialize_size(image, size) != size ||
       !offsets_canonical(reinterpret_cast<const std::uint8_t*>(image))) {
        return std::nullopt;
    }
    auto view = Bitmap::portableDeserializeFrozen(image);
    if(!roaring::api::roaring_bitmap_internal_validate(&view.roaring, nullptr)) {
        return std::nullopt;
    }
    return view;
}

}  // namespace clice::index
