/// # Deprecated
///
/// - status: supported
///
/// Deprecated declarations and uses carry the deprecated modifier

[[deprecated("use next_api")]] void §old_api();
void next_api();

void migrate() {
    §old_api();
}
