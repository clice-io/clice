struct Alpha {};

struct Beta {};

struct §(derived)Gamma : Alpha, Beta {};

using Base = Alpha;

struct §(alias_base)Delta : Base {};
