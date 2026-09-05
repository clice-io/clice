/// # `using` declaration blocks
///
/// - status: unsupported
///
/// Consecutive using declarations and directives do not form folding ranges yet

using std::vector;  // ┐
using std::string;  // │ foldable
using std::map;     // ┘
