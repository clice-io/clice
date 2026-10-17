# A definition whose declaration changed

Go-to-declaration on a definition that no longer matches its declaration reaches the declaration it parted from

When a parameter changes on one side only, the definition and the header
declaration become two functions, and the definition has no declaration of
its own. Go-to-declaration on it falls back to the functions of its name and
linkage in its scope that nothing defines: the declaration that wants the
same edit, a `static` one's in its own file. An overload with a definition
of its own is no candidate, nor is a function of another linkage or of an
anonymous namespace it is not in. A method defined in its class never had a
declaration apart, and a template or a system header's function is left
alone: those are declared where they are defined.
