/// # Macro Hover
///
/// ## Command-line macros — `-D` definitions hover with a synthesized `#define`
///
/// - status: supported
/// - order: 3
/// - flags: ["-DFROM_CLI=7"]
///
/// A macro defined on the command line (`-DFROM_CLI=7`) shows a synthesized
/// `#define FROM_CLI 7` in its hover card, then its expansion.

int cli = §(01_cli_use)FROM_CLI;
