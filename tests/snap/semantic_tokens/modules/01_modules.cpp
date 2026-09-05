/// # Module declarations
///
/// - status: supported
///
/// Module declarations tokenize contextual keywords, dotted names and private
/// fragments

module;

export module demo.core;

export int exported_value = 1;

module :private;

int private_value = 2;

#if 0
module :private;
#endif
