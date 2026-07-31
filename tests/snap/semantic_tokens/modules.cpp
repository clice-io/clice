/// # Modules
///
/// ## Module declarations — the contextual `module` keyword, dotted module names and the private fragment
///
/// - status: supported
/// - order: 1

module;

export module demo.core;

export int exported_value = 1;

module :private;

int private_value = 2;

#if 0
module :private;
#endif
