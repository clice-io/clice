# Changes restart the server

clice reads `clice.toml` when the server starts. Saving it makes the server ask the editor to restart it, and the edit applies in the new server

The rule gains the macro while the server runs; once the editor restarted the server, `main.cpp` compiles with it.
