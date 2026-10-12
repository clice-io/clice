// A friend class declaration names a class elsewhere; it is no member of
// the class that befriends it, declared before or not.

class Declared;

class Owner {
    friend class Declared;
    friend class Undeclared;
    int secret;
};
