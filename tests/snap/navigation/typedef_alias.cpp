/// # Go to Type Definition
///
/// ## Type aliases
///
/// - status: partial
/// - verify: server
/// - order: 5
///
/// Go-to-type-definition on a variable of an aliased type reaches the
/// `using` or `typedef` declaration; it does not yet unwrap the alias to
/// the underlying type's definition.

struct §(underlying)Impl {};

using §(alias)Handle = Impl;

typedef Impl LegacyHandle;

int use(Handle §(var)handle, LegacyHandle §(legacy)legacy) {
    return 0;
}
