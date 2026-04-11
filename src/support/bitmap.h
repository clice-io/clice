#pragma once

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <utility>

#define ROARING_EXCEPTIONS 0
#define ROARING_TERMINATE(message) std::abort()
#include "roaring/roaring.hh"

namespace clice {

using Bitmap = roaring::Roaring;

/// Compact bitmap optimized for small canonical-id sets.
///
/// Uses a tagged pointer scheme in a single uint64_t:
///   - Low bit 1: inline mode, bits 1-63 store the bitmap (ids 0-62)
///   - Low bit 0: heap mode, the value is a roaring::Roaring* (aligned, so low bit is 0)
///
/// Default state is inline-empty (data == 1).  Upgrades to heap when an id >= 63
/// is added.  The common case (< 63 canonical ids per MergedIndex) stays entirely
/// inline with zero heap allocation and single-instruction bitwise operations.
class ContextBitmap {
    constexpr static std::uint32_t inline_capacity = 63;
    constexpr static std::uint64_t inline_tag = 1;

    std::uint64_t data = inline_tag;

    bool is_inline() const {
        return data & inline_tag;
    }

    roaring::Roaring* as_heap() const {
        return reinterpret_cast<roaring::Roaring*>(data);
    }

    std::uint64_t bits() const {
        return data >> 1;
    }

    void free_heap() {
        if(!is_inline())
            delete as_heap();
    }

    void upgrade(std::uint32_t new_id) {
        auto* r = new roaring::Roaring();
        auto b = bits();
        while(b) {
            r->add(static_cast<std::uint32_t>(std::countr_zero(b)));
            b &= b - 1;
        }
        r->add(new_id);
        data = reinterpret_cast<std::uint64_t>(r);
    }

public:
    ContextBitmap() = default;

    ~ContextBitmap() {
        free_heap();
    }

    ContextBitmap(ContextBitmap&& other) noexcept : data(other.data) {
        other.data = inline_tag;
    }

    ContextBitmap& operator=(ContextBitmap&& other) noexcept {
        if(this != &other) {
            free_heap();
            data = other.data;
            other.data = inline_tag;
        }
        return *this;
    }

    ContextBitmap(const ContextBitmap&) = delete;
    ContextBitmap& operator=(const ContextBitmap&) = delete;

    void add(std::uint32_t id) {
        if(is_inline()) {
            if(id < inline_capacity) {
                data |= (1ULL << (id + 1));
            } else {
                upgrade(id);
            }
        } else {
            as_heap()->add(id);
        }
    }

    void remove(std::uint32_t id) {
        if(is_inline()) {
            if(id < inline_capacity)
                data &= ~(1ULL << (id + 1));
        } else {
            as_heap()->remove(id);
        }
    }

    bool is_empty() const {
        if(is_inline())
            return bits() == 0;
        return as_heap()->isEmpty();
    }

    /// Check if (this - other) has any bits set, without allocating in the common inline case.
    bool any_not_in(const ContextBitmap& other) const {
        if(is_inline() && other.is_inline())
            return (bits() & ~other.bits()) != 0;

        // Rare: at least one side is heap.  Use roaring operations.
        if(is_inline()) {
            auto b = bits();
            while(b) {
                auto i = static_cast<std::uint32_t>(std::countr_zero(b));
                if(!other.as_heap()->contains(i))
                    return true;
                b &= b - 1;
            }
            return false;
        }

        if(other.is_inline()) {
            for(auto v: *as_heap()) {
                if(v >= inline_capacity || !(other.bits() & (1ULL << v)))
                    return true;
            }
            return false;
        }

        return !(*as_heap() - *other.as_heap()).isEmpty();
    }

    friend bool operator==(const ContextBitmap& lhs, const ContextBitmap& rhs) {
        if(lhs.is_inline() && rhs.is_inline())
            return lhs.data == rhs.data;
        if(lhs.is_inline() != rhs.is_inline())
            return false;
        return *lhs.as_heap() == *rhs.as_heap();
    }

    /// Convert to roaring::Roaring (for serialization).
    roaring::Roaring to_roaring() const {
        roaring::Roaring r;
        if(is_inline()) {
            auto b = bits();
            while(b) {
                r.add(static_cast<std::uint32_t>(std::countr_zero(b)));
                b &= b - 1;
            }
        } else {
            r = *as_heap();
        }
        return r;
    }

    /// Construct from roaring::Roaring (for deserialization).
    static ContextBitmap from_roaring(const roaring::Roaring& r) {
        ContextBitmap result;
        if(r.isEmpty())
            return result;
        if(r.maximum() < inline_capacity) {
            for(auto v: r)
                result.data |= (1ULL << (v + 1));
        } else {
            result.data = reinterpret_cast<std::uint64_t>(new roaring::Roaring(r));
        }
        return result;
    }
};

}  // namespace clice
