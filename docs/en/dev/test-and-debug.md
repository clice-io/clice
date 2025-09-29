# Test and Debug

## Run Tests

clice has two types of tests: unit tests and integration tests.

- Run unit tests

```bash
$ ./build/bin/unit_tests --test-dir="./tests/data"
```

Or, run unit tests through ctest:

```bash
$ cd build
$ ctest -V
```

- Run integration tests

We use pytest to run integration tests. Please refer to `pyproject.toml` to install the required Python libraries.

```bash
$ pytest -s --log-cli-level=INFO tests/integration --executable=./build/bin/clice
```

If you use xmake as your build system, you can run the tests directly with xmake:

```shell
$ xmake run --verbose unit_tests
$ xmake test --verbose integration_tests/default
```

## Debug

If you want to attach a debugger to clice for debugging, it is recommended to first start clice in socket mode independently, and then connect the client to it.

```shell
$ ./build/bin/clice --mode=socket --port=50051
```

After the server starts, you can connect a client to the server in the following two ways:

- Connect by running a specific test with pytest

You can run a single integration test case to connect to a running clice instance. This is very useful for reproducing and debugging specific scenarios.

```shell
$ pytest -s --log-cli-level=INFO tests/integration/test_file_operation.py::test_did_open --mode=socket --port=50051
```

- Use VS Code for practical testing

You can also connect to a running clice service by configuring the clice-vscode extension, allowing you to debug in a real-world usage scenario.

1.  Download the [clice-vscode](https://marketplace.visualstudio.com/items?itemName=ykiko.clice-vscode) extension from the Marketplace.

2.  Configure `settings.json`: Create a `.vscode/settings.json` file in your project's root directory and add the following content:

    ```jsonc
    {
        // Point this to the clice binary you downloaded.
        "clice.executable": "/path/to/your/clice/executable",

        // Enable socket mode.
        "clice.mode": "socket",
        "clice.port": 50051,

        // Optional: Set this to an empty string to turn off the clangd.
        "clangd.path": "",
    }
    ```

3.  Reload Window: After modifying the configuration, execute the `Developer: Reload Window` command in VS Code for the settings to take effect. The extension will automatically connect to the clice instance listening on port 50051.


If you need to modify or debug the clice-vscode extension itself, follow these steps:

1.  Clone and install dependencies:
    ```shell
    $ git clone https://github.com/clice-io/clice-vscode
    $ cd clice-vscode
    $ npm install
    ```

2.  Open the extension project with VS Code: Open the `clice-vscode` folder in a new VS Code window.

3.  Create debug configuration: In the `clice-vscode` project, also create a `.vscode/settings.json` file with the same content as above.

4.  Press `F5`. This will launch an [Extension Development Host] window. This is a new VS Code window with your local clice-vscode extension code loaded. Open your C++ project in this new window, and it should automatically connect to clice.
