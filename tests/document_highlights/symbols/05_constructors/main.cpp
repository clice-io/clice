// The class name highlights wherever it names the class, inside `~Name` too;
// a constructor or destructor only its own declarations and uses, a
// construction at the class's name that calls the constructor.

struct §(class_decl)Session {
    §(ctor_decl)Session(int id);
    §(dtor_decl)~Session();
    int id;
};

Session::Session(int id) : id(id) {}

Session::~§(dtor_def)Session() {}

Session open() {
    return Session§(ctor_call)(7);
}

void close(Session& session) {
    session.§(dtor_call)~Session();
}
