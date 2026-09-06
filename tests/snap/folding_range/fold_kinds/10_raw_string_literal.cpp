/// # Raw string literal folding
///
/// - status: unsupported
///
/// Multiline raw string literals do not form folding ranges yet

auto sql = R"(
    SELECT *
    FROM users
    WHERE active = true
)";  // foldable multi-line raw string
