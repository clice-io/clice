/// # Module targets
///
/// - status: unsupported
///
/// `import` and `module` declarations link to their interface files

export module app;

import lib;
import :part;
export import lib.extra;
