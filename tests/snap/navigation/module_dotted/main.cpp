/// # Module Navigation
///
/// ## Dot-separated module name — navigate each segment
///
/// - status: partial
/// - verify: server
/// - order: 4
///
/// Go-to-definition on the leading segment of a dot-separated module name
/// reaches the module's interface unit; the segments after a dot do not
/// resolve on their own yet.

import §(seg_app)app.§(seg_core)core;

int run() {
    return value();
}
