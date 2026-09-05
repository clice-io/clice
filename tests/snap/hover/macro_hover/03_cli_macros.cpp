/// # Command-line macros
///
/// - status: supported
/// - flags: ["-DFROM_CLI=7"]
///
/// `-D` definitions hover with a synthesized `#define`
///
/// A macro defined on the command line (`-DFROM_CLI=7`) shows a synthesized
/// `#define FROM_CLI 7` in its hover card, then its expansion.

int cli = §(01_cli_use)FROM_CLI;
