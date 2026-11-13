// A comment inside an import is none of its module name, and a semicolon in a
// comment closes no import. Past the semicolon, the rest of the line is still
// the import directive's, which clang drops (-Wextra-tokens): nothing
// completes there.
import /* the math module; see math.cppm */ ma§(after_comment)
import ma§(closed_past_comment) /* the math module */ [[deprecated]];
import ma§(semicolon_in_comment) // see math.cppm; for its names
