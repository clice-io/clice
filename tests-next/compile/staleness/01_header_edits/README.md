# Changes outside the editor

A header changed on disk reaches the files that include it at their next request, saved or not

Checkouts, generators and other tools rewrite headers behind the editor's back. The next request about an including file compiles it against the new text, wherever the header sits in its includes, while a rewrite of the same bytes costs nothing.
