// - diagnostics: expected

// A member named through `.` or `->` belongs to the object's class: no
// header provides it, though the standard library has a name like it.

struct Box {};

int use(Box box, Box* pointer) {
    box.§(swap)swap(box);
    box.template §(get)get<0>();
    return pointer->§(count)count;
}
