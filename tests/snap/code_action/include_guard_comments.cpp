// - diagnostics: expected

// Comments around an include guard leave it a guard: the directive goes
// inside it, after its `#define`.

#ifndef FIXTURE_GUARD_H
#define FIXTURE_GUARD_H

using Strings = std::§(string)string;

#endif  // FIXTURE_GUARD_H
