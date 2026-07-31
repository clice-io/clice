/// # Declarations & References
///
/// ## Lambda init-captures — the captured name highlighted as a variable
///
/// - status: supported
/// - issues: clangd#868
/// - order: 11

int compute();

auto fn = [§val = compute()] {
    return §val;
};
