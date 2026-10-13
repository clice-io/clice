namespace headers {
/// Sums the elements of a range.
template <typename R>
    requires requires(R r) { r.size(); }
int sum(R&& r);

/// Picks one value.
template <typename T, typename = decltype(T{})>
T pick(T a);

/// A growable array.
template <typename T,  // the element
          typename A>  // the allocator
struct vec {};

struct Range {
    int size();
};

int use() {
    return §(constrained)sum(Range{}) + §(braced)pick(1);
}

§(class_template)vec<int, int>* p;
}  // namespace headers
