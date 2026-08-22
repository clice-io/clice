/// - verify: server

struct Session {
    §(ctor_decl)Session(int id);
    int id;
};

§(scope_use)Session::§(ctor_def)Session(int id) : id(id) {}

Session open() {
    return §(ctor_use)Session§(ctor_invoke)(7);
}
