# Errors in template instantiations

An error inside a template shows where the code asks for the instantiation

When a template fails for the arguments it is instantiated with, the error is reported on the code that requested the instantiation, not inside the template. Its notes lead through the chain of instantiations to the place in the template where it failed. A template in a header the file includes at its top is instantiated by the background index, not by the editor's compile, which skips the header's function bodies: its errors show once the index has compiled the file as saved.
