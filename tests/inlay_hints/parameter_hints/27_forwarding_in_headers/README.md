# Forwarding wrappers in headers

Wrappers a header defines resolve to their target's parameter names like those of the file itself

The editor skips the bodies of the functions in the headers a file includes at its top, but not those of the function templates whose last parameter is a pack of their own template parameters, as `std::make_unique`'s and `emplace_back`'s is: the call they forward to, whose parameters the hints name, is in that body.
