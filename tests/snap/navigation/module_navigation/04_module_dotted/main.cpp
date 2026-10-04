/// # Dot-separated module name
///
/// - status: supported
/// - verify: server
///
/// Every segment of a dotted module name navigates to its interface
///
/// Go-to-definition on any segment of a dot-separated module name reaches
/// the module's interface unit; the whole name is one reference.

import §(seg_app)app.§(seg_core)core;

int run() {
    return value();
}
