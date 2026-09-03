/// # Module targets — `import` and `module` declarations link to their interface files
///
/// - status: unsupported

export module app;

import lib;
import :part;
export import lib.extra;
