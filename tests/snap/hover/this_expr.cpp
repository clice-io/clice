namespace ordinary_class {
class Session {
    Session* current() {
        return t§(01_this_class)his;
    }
};
}

namespace class_template {
template <typename T> class Buffer {
    const Buffer* current() const {
        return t§(02_this_template_class)his;
    }
};
}

namespace full_specialization {
template <typename T> class Holder {};
template <> class Holder<long> {
    Holder* current() {
        return thi§(03_this_specialization)s;
    }
};
}

namespace partial_specialization {
template <typename T, typename U> struct Pair {};
template <typename U> struct Pair<long, U> {
    const Pair* current() const {
        return thi§(04_this_partial_specialization)s;
    }
};
}
