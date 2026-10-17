# Implicit constructor and destructor calls

Find references on a constructor or a destructor lists the places that name it, not the objects that run it implicitly

`Blob b;` runs the constructor, and the end of its scope the destructor, but
names neither: listing every object that does would bury the references of
any class with local variables.
