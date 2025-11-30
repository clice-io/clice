#pragma once

#include "Struct.h"

#include "llvm/Support/HashBuilder.h"

namespace clice::refl {

template <typename T>
struct Hash {
    static llvm::hash_code hash(const auto& value) {
        return llvm::hash_value(value);
    }
};

template <typename Value>
llvm::hash_code hash(const Value& value) {
    return Hash<Value>::hash(value);
}

template <typename T>
struct Hash<std::vector<T>> {
    static llvm::hash_code hash(const std::vector<T>& value) {
        llvm::SmallVector<llvm::hash_code, 8> hashes;
        hashes.reserve(value.size());
        for(const auto& element: value) {
            hashes.emplace_back(refl::hash(element));
        }
        return llvm::hash_combine_range(hashes.begin(), hashes.end());
    };
};

template <reflectable_struct T>
struct Hash<T> {
    static llvm::hash_code hash(const T& value) {
        llvm::SmallVector<llvm::hash_code, 8> hashes;
        foreach(value, [&](auto, const auto& member) { hashes.emplace_back(refl::hash(member)); });
        return llvm::hash_combine_range(hashes.begin(), hashes.end());
    }
};

}  // namespace clice::refl
