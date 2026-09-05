/// # Dot-separated module name
///
/// - status: partial
/// - verify: server
///
/// Only the leading segment of a dotted module name navigates to its interface
///
/// Go-to-definition on the leading segment of a dot-separated module name
/// reaches the module's interface unit; the segments after a dot do not
/// resolve on their own yet.

import §(seg_app)app.§(seg_core)core;

int run() {
    return value();
}
