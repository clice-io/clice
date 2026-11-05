template <class T>
struct Vec {
    int vec_member;
};

template <class T>
struct Box {
    template <class U = Vec<T>>
    U make();
};

template <class T>
int use(Box<T>& box) {
    return box.make().§(member)vec_member;
}
