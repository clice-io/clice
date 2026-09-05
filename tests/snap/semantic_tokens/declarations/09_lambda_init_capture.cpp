/// # Lambda init-captures
///
/// - status: supported
/// - issues: clangd#868
///
/// Lambda init-captures receive variable tokens

int compute();

auto fn = [§val = compute()] {
    return §val;
};
