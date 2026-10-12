# Changes need a restart

clice reads `clice.toml` when the server starts, so an edit applies at the next start

The rule's macro changes while the server runs, and `main.cpp` keeps the error until the server is started again.
