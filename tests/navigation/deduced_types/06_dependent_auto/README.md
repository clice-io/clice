# `auto` in templates

Inside a template, an `auto` whose initializer depends on a template
parameter reaches the type the initializer resolves to

A template parameter, whether of a function template or a class template,
stands for its argument when the template is instantiated once, and for
itself when it is instantiated with several types.
