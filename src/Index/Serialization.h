#include "schema_generated.h"
#include "Support/Bitmap.h"
#include "Support/Ranges.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice::index {

namespace fbs = flatbuffers;

namespace {

template <typename T>
using Offsets = llvm::SmallVector<fbs::Offset<T>, 0>;

template <typename U, typename V>
const U* safe_cast(const V* v) {
    static_assert(sizeof(U) == sizeof(V), "size mismatch");
    static_assert(alignof(U) == alignof(V), "alignment mismatch");
    static_assert(std::is_trivially_copyable_v<U> && std::is_trivially_copyable_v<V>,
                  "requires trivially copyable");
    /// If aliasing issues arise, prefer copying into a temporary SmallVector<U>.
    return reinterpret_cast<const U*>(v);
}

auto CreateString(fbs::FlatBufferBuilder& builder, llvm::StringRef string) {
    return builder.CreateString(string.data(), string.size());
}

template <sequence_range Range>
auto CreateVector(fbs::FlatBufferBuilder& builder, const Range& range) {
    return builder.CreateVector(range.data(), range.size());
}

auto CreateVector(fbs::FlatBufferBuilder& builder, const llvm::SmallVector<char, 1024>& range) {
    return builder.CreateVector(reinterpret_cast<const std::uint8_t*>(range.data()), range.size());
}

template <typename U, sequence_range Range>
auto CreateStructVector(fbs::FlatBufferBuilder& builder, const Range& range) {
    using V = ranges::range_value_t<Range>;
    return builder.CreateVectorOfStructs(safe_cast<U>(range.data()), range.size());
}

template <typename Range, typename Functor>
auto transform(const Range& range, const Functor& functor) {
    using V = ranges::range_value_t<Range>;
    using R = std::invoke_result_t<Functor, V>;

    llvm::SmallVector<R, 0> result;
    result.resize_for_overwrite(ranges::size(range));

    auto i = 0;
    for(auto&& v: range) {
        result[i] = functor(v);
        i += 1;
    }
    return result;
}

Bitmap read_bitmap(const fbs::Vector<uint8_t>* buffer) {
    return Bitmap::read(reinterpret_cast<const char*>(buffer->data()), false);
}

}  // namespace

}  // namespace clice::index
