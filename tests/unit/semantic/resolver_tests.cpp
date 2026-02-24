#include "semantic/resolver.h"

// NOTE:
// The original TemplateResolver behavior suite depends on the compiler pipeline
// (CompilationUnit/Tester). Keep this file as the migration anchor for now, and
// re-enable resolver behavior tests after compiler migration lands.
static_assert(sizeof(clice::TemplateResolver*) > 0);
