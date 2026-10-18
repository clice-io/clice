namespace std {

template <typename T>
struct allocator {};

template <typename T, typename Alloc = allocator<T>>
class vector {
public:
    typedef unsigned long size_type;
    size_type size() const;
};

}  // namespace std

void use(std::vector<int>& values) {
    §(keyword)auto §(variable)count = values.size();
}
