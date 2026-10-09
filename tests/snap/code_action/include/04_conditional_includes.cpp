/// # Includes under conditionals
///
/// - status: supported
/// - diagnostics: expected
///
/// An include nested in a feature condition is not where a directive that must always apply goes: it follows the last one at the file's own level, or the include guard

#ifndef FIXTURE_GUARD_H
#define FIXTURE_GUARD_H

#include "support.h"

#if FEATURE
#include "feature.h"
#endif

using Strings = std::§(string)string;

#endif
