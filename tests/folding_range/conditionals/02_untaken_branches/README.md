# Untaken branches

Untaken branches fold like taken ones

Every branch of a conditional folds whether or not the compile takes it,
conditionals nested in an untaken branch included, so dead code can be
folded away by hand. Untaken code is dimmed by the `inactive` modifier of
semantic tokens; the folds themselves do not tell the branches apart.
