# CTAD

A CTAD construction navigates to the deduced specialization's constructor

When class template argument deduction picks a specialization, go-to-
definition on the constructor call reaches the constructor that was
selected, not merely the class template.
