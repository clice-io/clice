/// # Raw string literal folding
///
/// - status: unsupported
/// - order: 9

auto sql = R"(
    SELECT *
    FROM users
    WHERE active = true
)";  // foldable multi-line raw string
