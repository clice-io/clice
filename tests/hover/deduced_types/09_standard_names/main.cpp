namespace std {

inline namespace __1 {

template <typename Char>
struct allocator {};

template <typename Char, typename Alloc = allocator<Char>>
struct basic_string {};

typedef basic_string<char> string;

namespace pmr {

template <typename Char>
struct polymorphic_allocator {};

typedef basic_string<char, polymorphic_allocator<char>> string;

}  // namespace pmr

}  // namespace __1

template <typename T>
struct vector {
    T& front();
};

}  // namespace std

typedef unsigned int __u32;

__u32 checksum();

void use(std::vector<std::string> names, std::vector<std::pmr::string> texts) {
    §(standard)auto& name = names.front();
    §(nested)auto& text = texts.front();
    §(header_alias)auto sum = checksum();
}
