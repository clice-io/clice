// Types private to a class spell out of line with the class's qualifier, in
// a template head and in a return type alike.

class Owner {
    enum Kind { A };
    struct Part {};

    template <Kind K>
    struct Slot {
        void §(head)f();
    };

    Part §(result)part();
};
