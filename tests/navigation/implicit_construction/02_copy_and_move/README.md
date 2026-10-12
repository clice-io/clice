# Copy/move construction and assignment

The `=` of an initialization or an assignment navigates to the function it
runs

Go-to-definition on the `=` of an assignment reaches the assignment
operator, and on the `=` of a copy- or move-initialization (`T b = a;`) the
copy or move constructor that builds the variable.
