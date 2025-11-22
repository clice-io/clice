#include "Support/StringPool.h"

namespace clice {

llvm::StringRef StringPool::save_cstr(llvm::StringRef str) {
    assert(!str.empty() && "expected non empty string");
    auto it = pooled_strs.find(str);
    if(it != pooled_strs.end()) {
        // If we already store the argument, reuse it.
        return *it;
    }

    // Allocate for new string.
    const auto size = str.size();
    auto ptr = allocator.Allocate<char>(size + 1);
    std::memcpy(ptr, str.data(), size);
    ptr[size] = '\0';

    llvm::StringRef cached{ptr, size};
    pooled_strs.insert(cached);
    return cached;
}

llvm::ArrayRef<const char*> StringPool::save_cstr_list(llvm::ArrayRef<const char*> list) {
    auto it = pooled_str_lists.find(list);

    /// If we already store the argument, reuse it.
    if(it != pooled_str_lists.end()) {
        return *it;
    }

    /// Allocate for new array.
    const auto size = list.size();
    auto ptr = allocator.Allocate<const char*>(size);
    std::ranges::copy(list, ptr);

    llvm::ArrayRef<const char*> cached{ptr, size};
    pooled_str_lists.insert(cached);
    return cached;
}

}  // namespace clice
