/// # Find References
///
/// ## Implicit constructor and destructor calls
///
/// - status: unsupported
/// - order: 4
///
/// Find references on a constructor reports only its explicit sites; an
/// object definition that implicitly invokes the constructor or its
/// destructor is not included.

struct Blob {
    Blob();  // find-refs here omits the `Blob b;` definition below
    ~Blob();
};

void use() {
    Blob b;
}
