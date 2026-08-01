/// # Fold Kinds
///
/// ## `using` declaration blocks — consecutive using declarations/directives
///
/// - status: unsupported
/// - order: 11

using std::vector;  // ┐
using std::string;  // │ foldable
using std::map;     // ┘
