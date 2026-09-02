/// # Deprecated — `[[deprecated]]` declarations and their uses
///
/// - status: supported

[[deprecated("use next_api")]] void §old_api();
void next_api();

void migrate() {
    §old_api();
}
