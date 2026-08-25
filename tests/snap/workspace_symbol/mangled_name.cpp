/// # Workspace Symbol
///
/// ## Search by mangled (linker) name
///
/// - status: unsupported
/// - order: 8
///
/// Pasting a linker symbol such as `_Z7processi` should resolve to the
/// function it mangles — useful when chasing linker errors and stack
/// traces.

// query: _Z7processi

void process(int value);
