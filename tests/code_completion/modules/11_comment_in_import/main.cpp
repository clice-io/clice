// A comment inside an import is none of its module name, and a semicolon in a
// comment closes no import; past the semicolon, the line completes names.
import /* the math module; see math.cppm */ ma§(after_comment)
import ma§(closed_past_comment) /* the math module */ [[deprecated]];
import ma§(semicolon_in_comment) // see math.cppm; for its names
