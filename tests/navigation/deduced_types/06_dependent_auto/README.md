# `auto` in uninstantiated templates

Inside a template, an `auto` whose initializer depends on a template
parameter reaches the type the initializer resolves to on the class
template

A type that is the template parameter itself reaches the parameter, however
the template is instantiated.
