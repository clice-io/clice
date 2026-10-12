# Macro-generated names

A name a macro creates resolves to the invocation that creates it

A use of a name assembled by token paste, or spelled in the macro's body,
reaches the invocation, and the invocation's own name resolves to the
macro. A token inside the macro's body has no meaning until an expansion
gives it one, so navigation on it answers nothing.
