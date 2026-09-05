/// # Call hierarchy preparation
///
/// - status: supported
/// - verify: server
///
/// Preparing a call hierarchy works on a free function and on a member method
/// alike, anchoring an item at the entity under the cursor

struct Service {
    void §(method)start();
};

void Service::start() {}

void §(func)launch(Service& s) {
    s.start();
}
