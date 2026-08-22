/// - verify: server

struct Resource {
    §(dtor_decl)~Resource();
};

Resource::§(dtor_def)~Resource() {}

void release(Resource& resource) {
    resource.§(dtor_use)~Resource();
}
