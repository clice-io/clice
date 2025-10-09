# Test and Debug

## Run Tests

clice 有两种形式的测试，单元测试和集成测试。

- 运行单元测试

```bash
$ ./build/bin/unit_tests --test-dir="./tests/data"
```

- 运行集成测试

我们使用 pytest 来运行集成测试，请参考 `pyproject.toml` 安装依赖的 python 库

```bash
$ pytest -s --log-cli-level=INFO tests/integration --executable=./build/bin/clice
```

如果你使用 xmake 作为构建系统，可以直接通过 xmake 运行测试：

```shell
$ xmake run --verbose unit_tests
$ xmake test --verbose integration_tests/default
```

## Debug

如果想在 clice 上附加调试器并进行调试，推荐先单独以 socket 模式启动 clice，然后再将客户端连接到 clice 上

```shell
$ ./build/bin/clice --mode=socket --port=50051
```

在服务器启动之后，可以通过以下两种方式启动客户端连接到服务器

- 使用 pytest 运行特定测试进行连接

你可以运行一个单独的集成测试用例来连接正在运行的 clice。这对于复现和调试特定场景非常有用。

```shell
$ pytest -s --log-cli-level=INFO tests/integration/test_file_operation.py::test_did_open --mode=socket --port=50051
```

- 使用 vscode 进行实际的测试

你也可以通过配置 clice-vscode 插件来连接正在运行的 clice 服务，从而在实际使用场景中进行调试。

1. 在插件市场下载插件 [clice-vscode](https://marketplace.visualstudio.com/items?itemName=ykiko.clice-vscode)

2. 配置 `settings.json`: 在你的项目根目录下创建 `.vscode/settings.json` 文件，并填入以下内容：

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

3. 重新加载窗口：修改配置后，在 vscode 中执行 Developer: Reload Window 命令使配置生效。插件会自动连接到正在 50051 端口监听的 clice。


如果你需要修改或调试 clice-vscode 插件本身，可以按以下步骤操作：

1. 克隆并安装依赖：
    ```shell
    $ git clone https://github.com/clice-io/clice-vscode
    $ cd clice-vscode
    $ npm install
    ```

2. 使用 vscode 打开插件项目：用一个新的 vscode 窗口打开 clice-vscode 文件夹

3. 创建调试配置：在 clice-vscode 项目中，也创建一个 `.vscode/settings.json` 文件，内容与上方相同

4. 按下 `F5` 键。这会启动一个【扩展开发宿主】窗口。这是一个加载了你本地 clice-vscode 插件代码的新的 vscode 窗口，在这个新窗口中打开你的 C++ 项目，它应该会自动连接到 clice
