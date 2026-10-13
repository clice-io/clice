// A structured binding pack shows its declaration with the pack's ellipsis.

namespace binding_packs {

struct Triple {
    int first;
    long second;
    char third;
};

template <typename T> int count(T value) {
    auto [§(head)head, ...tail] = value;
    return sizeof...(tail);
}

int total = count(Triple{});

}
