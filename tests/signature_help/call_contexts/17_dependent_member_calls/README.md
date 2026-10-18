# Dependent member calls

Inside a template, a call through an object of dependent type shows the overloads its class template declares

Past the first argument only the overloads taking another one stay. Clang has
no signature for such a call until the template is instantiated.
