# A definition whose declaration changed

Go-to-declaration on a definition that no longer matches its declaration reaches the declaration it parted from

When a parameter changes on one side only, the definition and the header
declaration become two functions, and the definition has no declaration of
its own. Go-to-declaration on it falls back to the functions of its name in
its scope that nothing defines: the declaration that wants the same edit. An
overload with a definition of its own is no candidate.
