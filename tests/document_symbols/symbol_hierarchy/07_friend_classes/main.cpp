// A friend declaration names a class or a function elsewhere; it is no
// member of the class that befriends it, declared before or not.

class Declared;

class Owner {
    friend class Declared;
    friend class Undeclared;
    friend void notify(Owner& owner);
    int secret;
};
