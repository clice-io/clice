# Uses inside templates and macros

Find references and the call hierarchy include what a template's dependent code and a macro's body resolve to wherever a source instantiates or expands them, open or not

`item.open()` in `measure` names `Box::open` only once `measure` is
instantiated with `Box`, and `reset()` in the body of `RESET_ALL` names the
function only where the macro expands: here both happen in a source that is
not open, and still answer while the headers are open.
