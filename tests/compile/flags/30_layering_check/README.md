# Layering check

An include the build's layering check rejects shows no error

Bazel's `layering_check`, and any build that hands Clang module maps, rejects an include of a header that no dependency of the target provides. clice compiles a file as if no module map named it: such an include compiles clean in the editor, and only the build reports it.
