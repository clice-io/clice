/// # Workspace Symbol
///
/// ## Partially qualified name search
///
/// - status: unsupported
/// - order: 5
/// - issues: clangd#550
///
/// Symbols match by bare name only: `net::Socket` finds nothing even
/// though `deep::net::Socket` exists, and neither does any other
/// qualifier-prefixed form.

// query: net::Socket

namespace deep {
namespace net {

struct Socket {};

}  // namespace net
}  // namespace deep
