namespace geometry {

template <class T>
struct Box {
    using value_type = T;
};

}  // namespace geometry

geometry::Box<§(argument)int> box;

typename geometry::Box<int>::§(member)value_type size = 0;

template <class T>
void fill() {
    typename geometry::Box<T>::§(dependent)value_type value{};
    (void)value;
}
