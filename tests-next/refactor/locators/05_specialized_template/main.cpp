// A template with specializations: its name alone names the template, to
// the query and to the rename, which renames every specialization with it.
template <typename T>
struct Holder {
    Holder(T value) : value(value) {}
    T value;
};

template <typename T>
struct Holder<T*> {
    Holder(T* pointer) : pointer(pointer) {}
    T* pointer;
};

template <>
struct Holder<bool> {
    Holder(bool flag) : flag(flag) {}
    bool flag;
};

Holder(const char*) -> Holder<const char*>;

Holder number{1};
Holder<bool> yes{true};
int target = 0;
Holder<int*> pointer{&target};
Holder text{"text"};
