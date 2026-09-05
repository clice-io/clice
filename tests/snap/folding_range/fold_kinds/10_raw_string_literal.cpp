/// # Raw string literal folding
///
/// - status: unsupported
///
/// Multiline raw string literals form folding ranges

auto sql = R"(
    SELECT *
    FROM users
    WHERE active = true
)";  // foldable multi-line raw string
