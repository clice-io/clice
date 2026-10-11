# Search spans the whole project

Workspace symbol search returns hits from project files that are not open

The query finds a function in `other.cpp`, which stays closed while
`main.cpp` is open: the background index answers for it.
