/// # Fold Kinds
///
/// ## Raw string literal folding
///
/// - status: unsupported
/// - order: 10

auto sql = R"(
    SELECT *
    FROM users
    WHERE active = true
)";  // foldable multi-line raw string
