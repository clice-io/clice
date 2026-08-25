/// # Call Hierarchy
///
/// ## Prepare call hierarchy on functions and methods
///
/// - status: supported
/// - verify: server
/// - order: 1
///
/// Preparing a call hierarchy works on a free function and on a member
/// method alike, anchoring an item at the entity under the cursor.

struct Service {
    void §(method)start();
};

void Service::start() {}

void §(func)launch(Service& s) {
    s.start();
}
