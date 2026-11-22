struct §(resource)Resource {
    Resource(int id) : id(id) {}
    ~Resource() {}

    int id;
};

struct Pinned {
    Pinned() = default;
    Pinned(const Pinned&) = default;
    Pinned(Pinned&&) = delete;
};

struct §(holder)Holder {
    Holder(int id) : id(id) {}

    Pinned pinned;
    int id;
};

Resource make(int id) {
    Resource resource(id);
    return resource;
}

void use(Resource& resource, Holder& holder) {
    Resource copy = static_cast<Resource&&>(resource);
    copy = static_cast<Resource&&>(resource);
    Holder other = static_cast<Holder&&>(holder);
}
