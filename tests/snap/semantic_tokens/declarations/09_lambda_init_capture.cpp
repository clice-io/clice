/// # Lambda init-captures
///
/// - status: supported
/// - issues: clangd#868
///
/// The captured name highlighted as a variable

int compute();

auto fn = [§val = compute()] {
    return §val;
};
