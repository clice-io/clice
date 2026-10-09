// - diagnostics: expected

// A module unit without a global module fragment opens one for the
// directive: no include may follow the module declaration.

export module demo;

export using Strings = std::§(string)string;
