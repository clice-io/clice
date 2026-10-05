/// # Declarator-shaped field types
///
/// - status: supported
///
/// A field whose type wraps the name, such as a function pointer, keeps that shape in the parameter

struct §(callbacks)Callbacks {
    void (*on_open)(int);
    int (&counter)[4];
    const char* name;
};
