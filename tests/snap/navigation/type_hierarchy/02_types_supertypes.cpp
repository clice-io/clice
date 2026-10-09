/// # Supertypes
///
/// - status: supported
/// - verify: server
///
/// Supertypes list every direct base of a class, including each base of a
/// multiple-inheritance derived type
///
/// A base written through an alias lists the class the alias names.

struct Alpha {};

struct Beta {};

struct §(derived)Gamma : Alpha, Beta {};

using Base = Alpha;

struct §(alias_base)Delta : Base {};
