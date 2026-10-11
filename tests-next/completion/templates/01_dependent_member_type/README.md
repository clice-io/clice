# Dependent member type

A variable whose type is a member alias of a dependent specialization completes the members of the class the alias stands for

The alias is resolved with the written template arguments substituted, so `Vec<Vec<T>>::value_type` lists the members of `Vec<T>` rather than nothing at all. An alias naming a reference (`Vec<Vec<T>>::reference`) lists the members of the class referred to, and `->` on a pointer to one lists the pointee's.
