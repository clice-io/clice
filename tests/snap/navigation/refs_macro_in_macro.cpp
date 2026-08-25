/// # Find References
///
/// ## Macro references spelled inside other macro definitions
///
/// - status: unsupported
/// - order: 10
/// - issues: clangd#346
///
/// Find references on a macro does not include the mentions of it written
/// inside the bodies of other macro definitions.

#define WIDTH 100  // find-refs here omits the WIDTH tokens in AREA below

#define AREA (WIDTH * WIDTH)

int total = AREA;
