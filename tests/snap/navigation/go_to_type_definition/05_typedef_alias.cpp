/// # Type aliases
///
/// - status: partial
/// - verify: server
///
/// Go-to-type-definition on a variable of an aliased type reaches the `using`
/// or `typedef` declaration; it does not yet unwrap the alias to the underlying
/// type's definition

struct §(underlying)Impl {};

using §(alias)Handle = Impl;

typedef Impl LegacyHandle;

template <typename T>
struct Box {};

using §(template_alias)Boxed = Box<int>;

int use(Handle §(var)handle, LegacyHandle §(legacy)legacy, Boxed §(boxed)boxed) {
    return 0;
}
