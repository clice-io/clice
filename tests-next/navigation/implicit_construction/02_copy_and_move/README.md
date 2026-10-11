# Copy/move construction and assignment

Assignment `=` navigates to the assignment operator, while copy and move
initialization do not

Go-to-definition on the `=` of an assignment reaches the assignment
operator. The `=` that introduces a copy- or move-initialization
(`T b = a;`) is initialization syntax rather than an operator call and is
not yet resolved.
