module;

#include "modules/prelude.h"

module clice;

import :index.serialization;

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

/// The low bits each id of a posting list of `count` ids keeps.
std::uint32_t low_bits(std::uint64_t count, std::uint32_t universe) {
    return count == 0 ? 0 : std::bit_width(universe / count) - 1;
}

/// Bits appended least significant first, a byte at a time.
struct BitWriter {
    std::vector<std::uint8_t>& out;
    std::uint64_t buffer = 0;
    std::uint32_t held = 0;

    void put(std::uint64_t bits, std::uint32_t width) {
        buffer |= bits << held;
        held += width;
        while(held >= 8) {
            out.push_back(static_cast<std::uint8_t>(buffer));
            buffer >>= 8;
            held -= 8;
        }
    }

    void flush() {
        if(held != 0) {
            out.push_back(static_cast<std::uint8_t>(buffer));
            buffer = 0;
            held = 0;
        }
    }
};

/// Bits read in the order BitWriter appends them.
struct BitReader {
    llvm::ArrayRef<std::uint8_t> bytes;
    std::size_t at = 0;
    std::uint64_t buffer = 0;
    std::uint32_t held = 0;

    std::uint32_t take(std::uint32_t width) {
        while(held < width) {
            buffer |= std::uint64_t(bytes[at]) << held;
            at += 1;
            held += 8;
        }
        auto value = static_cast<std::uint32_t>(buffer & ((std::uint64_t(1) << width) - 1));
        buffer >>= width;
        held -= width;
        return value;
    }
};

}  // namespace

std::vector<std::uint8_t> write_postings(const Bitmap& ids, std::uint32_t universe) {
    std::vector<std::uint8_t> out;
    auto count = ids.cardinality();
    for(auto rest = count;;) {
        auto byte = static_cast<std::uint8_t>(rest & 0x7f);
        rest >>= 7;
        out.push_back(rest == 0 ? byte : byte | 0x80);
        if(rest == 0) {
            break;
        }
    }
    auto low = low_bits(count, universe);
    BitWriter lows{.out = out};
    for(auto id: ids) {
        lows.put(id & ((std::uint64_t(1) << low) - 1), low);
    }
    lows.flush();
    BitWriter highs{.out = out};
    std::uint64_t position = 0;
    std::uint64_t i = 0;
    for(auto id: ids) {
        auto one = (std::uint64_t(id) >> low) + i;
        while(position < one) {
            auto zeros = static_cast<std::uint32_t>(std::min<std::uint64_t>(one - position, 32));
            highs.put(0, zeros);
            position += zeros;
        }
        highs.put(1, 1);
        position += 1;
        i += 1;
    }
    highs.flush();
    return out;
}

std::optional<Bitmap> read_postings(llvm::ArrayRef<std::uint8_t> bytes, std::uint32_t universe) {
    std::uint64_t count = 0;
    std::size_t at = 0;
    for(std::uint32_t shift = 0;; shift += 7) {
        if(at == bytes.size() || shift > 28) {
            return std::nullopt;
        }
        auto byte = bytes[at];
        at += 1;
        count |= std::uint64_t(byte & 0x7f) << shift;
        if((byte & 0x80) == 0) {
            if(byte == 0 && at > 1) {
                return std::nullopt;
            }
            break;
        }
    }
    if(count > universe) {
        return std::nullopt;
    }
    auto low = low_bits(count, universe);
    auto low_bits_total = count * low;
    auto low_bytes = (low_bits_total + 7) / 8;
    if(bytes.size() - at < low_bytes) {
        return std::nullopt;
    }
    auto lows = bytes.slice(at, low_bytes);
    auto highs = bytes.drop_front(at + low_bytes);
    if(low_bits_total % 8 != 0 && (lows.back() >> (low_bits_total % 8)) != 0) {
        return std::nullopt;
    }
    if(count == 0) {
        return highs.empty() ? std::optional(Bitmap()) : std::nullopt;
    }
    if(highs.empty() || highs.back() == 0) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> ids;
    ids.reserve(count);
    BitReader reader{.bytes = lows};
    for(std::size_t b = 0; b < highs.size(); b += 1) {
        for(std::uint8_t byte = highs[b]; byte != 0; byte &= byte - 1) {
            if(ids.size() == count) {
                return std::nullopt;
            }
            std::uint64_t high = b * 8 + std::countr_zero(byte) - ids.size();
            if(high > (universe >> low)) {
                return std::nullopt;
            }
            auto id = (high << low) | reader.take(low);
            if(id >= universe || (!ids.empty() && id <= ids.back())) {
                return std::nullopt;
            }
            ids.push_back(static_cast<std::uint32_t>(id));
        }
    }
    if(ids.size() != count) {
        return std::nullopt;
    }
    Bitmap result;
    result.addMany(ids.size(), ids.data());
    return result;
}

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
