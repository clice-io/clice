/// Logs a message.
/// @param format The printf format.
/// @param ... The values it formats.
void log_message(const char* format, ...);

/// Opens a file.
/// @param __path The file to open.
/// @param __flags How to open it.
int open_file(const char* __path, int __flags);

/// Copies bytes.
/// @param to Where they go.
/// @param from Where they come from.
void copy_bytes(char* to, const char* from);

void copy_bytes(char* destination, const char* source) {}

/// Logs through the macro.
/// @param format The printf format.
/// @param ... The values it formats.
#define LOG_ALL(format, ...) log_message(format, __VA_ARGS__)

int main() {
    log_message("%d", §(variadic)1);
    open_file(§(uglified)"a", 0);
    copy_bytes(nullptr, §(redeclared)"x");
    LOG_ALL("%d", §(macro_variadic)1);
}
