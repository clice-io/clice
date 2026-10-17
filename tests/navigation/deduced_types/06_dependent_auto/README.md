# `auto` in templates

Inside a template, an `auto` whose initializer depends on a template
parameter reaches the type the initializer resolves to

An `auto` whose type is a template parameter, of a function template or a
class template, reaches the parameter and its argument in each
instantiation.
