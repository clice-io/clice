#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Enum.h"
#include "FixedString.h"
#include "Format.h"
#include "Struct.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice::binary {

template <typename T>
struct array {
    /// The offset to the beginning of binary buffer.
    uint32_t offset;

    /// The size of array.
    uint32_t size;
};

using string = array<char>;

/// Check whether a type can be directly binarized.
template <typename T>
constexpr inline bool is_directly_binarizable_v = [] {
    if constexpr(std::is_integral_v<T> || refl::reflectable_enum<T>) {
        return true;
    } else if constexpr(refl::reflectable_struct<T>) {
        return refl::member_types<T>::apply(
            []<typename... Ts>() { return (is_directly_binarizable_v<Ts> && ...); });
    } else {
        return false;
    }
}();

template <typename T>
consteval auto binarify();

template <typename T>
using binarify_t = typename decltype(binarify<T>())::type;

template <typename T>
consteval auto binarify() {
    if constexpr(is_directly_binarizable_v<T>) {
        return identity<T>();
    } else if constexpr(std::is_same_v<T, std::string>) {
        return identity<binary::string>();
    } else if constexpr(is_specialization_of<T, std::vector>) {
        return identity<binary::array<typename T::value_type>>();
    } else if constexpr(is_specialization_of<T, std::tuple>) {
        return tuple_to_list_t<T>::apply(
            []<typename... Ts> { return identity<std::tuple<binarify_t<Ts>...>>(); });
    } else if constexpr(refl::reflectable_struct<T>) {
        return refl::member_types<T>::apply(
            []<typename... Ts> { return identity<std::tuple<binarify_t<Ts>...>>(); });
    } else {
        static_assert(dependent_false<T>, "unsupported type");
    }
}

/// A section in the binary data.
template <typename T>
struct Section {
    /// Current count of elements.
    uint32_t count = 0;

    /// Total count of elements in the section.
    uint32_t total = 0;

    /// Offset of the section.
    uint32_t offset = 0;
};

template <typename T, typename Primary = void>
consteval auto layout() {
    if constexpr(is_directly_binarizable_v<T>) {
        return std::tuple<>();
    } else if constexpr(std::is_same_v<T, std::string>) {
        return std::tuple<Section<char>>();
    } else if constexpr(is_specialization_of<T, std::vector>) {
        using V = typename T::value_type;
        if constexpr(std::is_same_v<V, Primary>) {
            return std::tuple<Section<V>>();
        } else {
            return std::tuple_cat(std::tuple<Section<V>>(), layout<V>());
        }
    } else if constexpr(is_specialization_of<T, std::tuple>) {
        return tuple_to_list_t<T>::apply(
            []<typename... Ts> { return std::tuple_cat(layout<Ts>()...); });
    } else if constexpr(refl::reflectable_struct<T>) {
        return refl::member_types<T>::apply(
            []<typename... Ts> { return std::tuple_cat(layout<Ts, T>()...); });
    } else {
        static_assert(dependent_false<T>, "unsupported type");
    }
}

/// Get the binary layout of a type. Make sure every type in the
/// layout is unique.
template <typename T>
using layout_t = tuple_uniuqe_t<decltype(layout<T>())>;

template <typename T>
struct Packer {
    /// The layout of the binary data.
    layout_t<T> layout = {};

    /// The total size of the binary data.
    uint32_t size = 0;

    /// The buffer to store the binary data.
    std::vector<char> buffer;

    /// Recursively traverse the object and calculate the size of each section.
    template <typename Object>
    void init(const Object& object) {
        if constexpr(std::same_as<Object, std::string>) {
            std::get<Section<char>>(layout).total += object.size() + 1;
        } else if constexpr(requires { typename Object::value_type; }) {
            std::get<Section<typename Object::value_type>>(layout).total += object.size();
            for(const auto& element: object) {
                init(element);
            }
        } else if constexpr(refl::reflectable_struct<Object>) {
            refl::foreach(object, [&](auto, auto& field) { init(field); });
        }
    }

    template <typename Object>
        requires (is_directly_binarizable_v<Object> && !refl::reflectable_struct<Object>)
    Object write(const Object& object) {
        return object;
    }

    template <typename Object>
        requires (std::is_same_v<Object, std::string>)
    string write(const Object& object) {
        auto& section = std::get<Section<char>>(layout);
        uint32_t size = object.size();
        uint32_t offset = section.offset + section.count;
        section.count += size + 1;

        std::memcpy(buffer.data() + offset, object.data(), size);
        buffer[offset + size] = '\0';

        return string{offset, size};
    }

    template <typename Object, typename V = typename Object::value_type>
        requires (is_specialization_of<Object, std::vector>)
    array<V> write(const Object& object) {
        auto& section = std::get<Section<V>>(layout);
        uint32_t size = object.size();
        uint32_t offset = section.offset + section.count * sizeof(binarify_t<V>);
        section.count += size;

        for(std::size_t i = 0; i < size; ++i) {
            ::new (buffer.data() + offset + i * sizeof(binarify_t<V>)) auto{write(object[i])};
        }

        return array<V>{offset, size};
    }

    template <typename Object>
        requires (refl::reflectable_struct<Object>)
    std::array<char, sizeof(binarify_t<Object>)> write(const Object& object) {
        std::array<char, sizeof(binarify_t<Object>)> buffer;
        std::memset(buffer.data(), 0, sizeof(buffer));

        binarify_t<Object> result;
        refl::foreach(result, object, [&](auto& lhs, auto& rhs) {
            auto offset = reinterpret_cast<char*>(&lhs) - reinterpret_cast<char*>(&result);
            ::new (buffer.data() + offset) auto{write(rhs)};
        });

        return buffer;
    }

    std::vector<char> pack(const auto& object) {
        /// First initialize the layout.
        init(object);

        /// Calculate the total size of the binary data and
        /// the offset of each section.
        size = sizeof(binarify_t<T>);

        auto try_each = [&]<typename V>(auto, Section<V>& field) {
            static_assert(alignof(binarify_t<V>) <= 8, "Alignment not supported.");

            /// Make sure each section is aligned to 8 bytes.
            if(size % 8 != 0) {
                size += 8 - size % 8;
            }

            field.offset = size;
            size += field.total * sizeof(binarify_t<V>);
        };

        refl::foreach(layout, try_each);

        /// Make sure the buffer is clean. So we can compare the result.
        /// Every padding in the struct should be filled with 0.
        buffer.resize(size, 0);

        /// Write the object to the buffer.
        auto result = write(object);
        std::memcpy(buffer.data(), &result, sizeof(result));

        return std::move(buffer);
    }
};

/// A helper class to access the binary data.
template <typename T>
struct Proxy {
    using underlying_type = binarify_t<T>;
    const void* base;
    const void* data;

    const auto& value() const {
        return *reinterpret_cast<const underlying_type*>(data);
    }

    template <std::size_t I>
    auto get() const {
        return Proxy<refl::member_type<T, I>>{base, &std::get<I>(value())};
    }

    template <fixed_string name>
    auto get() const {
        constexpr auto& names = refl::member_names<T>();

        constexpr auto index = []() {
            for(std::size_t i = 0; i < names.size(); ++i) {
                if(names[i] == name) {
                    return i;
                }
            }
            return names.size();
        }();

        return this->template get<index>();
    }

    auto as_string() const {
        auto [offset, size] = value();
        return llvm::StringRef{reinterpret_cast<const char*>(base) + offset, size};
    }

    auto as_array() const {
        auto [offset, size] = value();
        using U = binarify_t<typename T::value_type>;
        return llvm::ArrayRef<U>{
            reinterpret_cast<const U*>(static_cast<const char*>(base) + offset),
            size,
        };
    }

    auto operator[](std::size_t index) const {
        return Proxy<typename T::value_type>{base, &as_array()[index]};
    }

    auto size() const {
        return value().size;
    }

    auto operator->() const {
        return &value();
    }

    operator const underlying_type&() const {
        return value();
    }
};

/// Binirize an object.
template <typename Object>
auto serialize(const Object& object) {
    auto buffer = Packer<Object>().pack(object);
    auto proxy = Proxy<Object>{buffer.data(), buffer.data()};
    return std::tuple(std::move(buffer), proxy);
}

template <typename Object>
Object deserialize(Proxy<Object> proxy) {
    if constexpr(is_directly_binarizable_v<Object>) {
        return proxy.value();
    } else if constexpr(std::is_same_v<Object, std::string>) {
        return proxy.as_string().str();
    } else if constexpr(is_specialization_of<Object, std::vector>) {
        Object result;
        for(std::size_t i = 0; i < proxy.size(); i++) {
            result.emplace_back(deserialize(proxy[i]));
        }
        return result;
    } else if constexpr(refl::reflectable_struct<Object>) {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return Object{deserialize(proxy.template get<Is>())...};
        }(std::make_index_sequence<refl::member_count<Object>()>());
    } else {
        static_assert(dependent_false<Object>, "");
    }
}

}  // namespace clice::binary
