# `using` declaration runs

Consecutive using declarations and directives fold below the first one

A blank line or any other line ends the run, and alias declarations do not
join one, nor does a declaration sharing its line with other code. Using
declarations produced by macros fold at the invocations.
