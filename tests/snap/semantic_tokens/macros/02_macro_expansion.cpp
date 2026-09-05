/// # Expansion sites and arguments
///
/// - status: supported
///
/// Expansion names are macros, written arguments keep their semantics, definition bodies stay lexical

int value = 1;

#define ID(x) x
#define CALL §helper()

void helper();

int copied = §ID(§value);

void run() {
    §CALL;
}
