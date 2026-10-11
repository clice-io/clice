[[deprecated("use next_api")]] void §(old_api_decl)old_api();
void next_api();

void migrate() {
    §(old_api_use)old_api();
}
