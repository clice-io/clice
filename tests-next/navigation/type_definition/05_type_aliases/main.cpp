struct §(underlying)Impl {};

using §(alias)Handle = Impl;

typedef Impl LegacyHandle;

template <typename T>
struct Box {};

using §(template_alias)Boxed = Box<int>;

int use(Handle §(var)handle, LegacyHandle §(legacy)legacy, Boxed §(boxed)boxed) {
    return 0;
}
