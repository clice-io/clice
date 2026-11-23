struct §(resource)Resource {
    Resource(int id) : id(id) {}
    ~Resource() {}

    int id;
};

struct §(assigned)Assigned {
    Assigned(int id) : id(id) {}
    Assigned& operator=(const Assigned& other) {
        id = other.id;
        return *this;
    }

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

void use(Resource& resource, Assigned& assigned, Holder& holder) {
    Resource copy = static_cast<Resource&&>(resource);
    copy = static_cast<Resource&&>(resource);
    Assigned other = static_cast<Assigned&&>(assigned);
    other = static_cast<Assigned&&>(assigned);
    Holder moved = static_cast<Holder&&>(holder);
}
