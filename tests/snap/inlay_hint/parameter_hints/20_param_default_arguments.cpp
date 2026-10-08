/// # Default argument names
///
/// - status: supported
/// - config: {"default_arguments": true}
///
/// The parameter names in a default-argument hint link to their parameters,
/// as parameter name hints do

void log(int level, bool flush = true, int repeat = 1);

void use() {
    log(2);
}
