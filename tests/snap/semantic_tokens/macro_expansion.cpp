/// # Macros
///
/// ## Expansion sites and arguments — expansion names are macros, written arguments keep their semantics, definition bodies stay lexical
///
/// - status: supported
/// - order: 2

int value = 1;

#define ID(x) x
#define CALL §helper()

void helper();

int copied = §ID(§value);

void run() {
    §CALL;
}
