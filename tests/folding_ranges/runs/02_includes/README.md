# Include runs

Consecutive include directives fold as one run below the first include

Blank lines and comment lines between includes stay in the run; code or
another directive ends it. Includes inside a conditional branch fold within
it, whether or not the branch is taken.
