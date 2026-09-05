/// # Raw string literal folding
///
/// - status: unsupported
///

auto sql = R"(
    SELECT *
    FROM users
    WHERE active = true
)";  // foldable multi-line raw string
