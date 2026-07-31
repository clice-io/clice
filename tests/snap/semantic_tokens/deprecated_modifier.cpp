/// # Token Modifiers
///
/// ## Deprecated — `[[deprecated]]` declarations and their uses
///
/// - status: supported
/// - order: 5

[[deprecated("use next_api")]] void §old_api();
void next_api();

void migrate() {
    §old_api();
}
