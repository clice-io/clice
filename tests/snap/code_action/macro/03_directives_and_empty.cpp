/// # Directive references and empty macros
///
/// - status: supported
///
/// A macro named in a preprocessor condition is not an expansion to replace, while a macro expanding to nothing is deleted

#define GUARD 1
#define EMPTY

#if §(directive)GUARD
int guarded = §(empty)EMPTY 1;
#endif
