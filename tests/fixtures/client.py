from pathlib import Path
from .transport import LSPTransport


class OpeningFile:
    def __init__(self, content: str):
        self.version = 0
        self.content = content


class LSPClient(LSPTransport):
    def __init__(self, commands, mode, host, port):
        super().__init__(commands, mode, host, port)
        self.workspace = ""
        self.opening_files: dict[Path, OpeningFile] = {}

    async def initialize(self, workspace: str):
        self.workspace = workspace
        params = {
            "clientInfo": {
                "name": "clice tester",
                "version": "0.0.1",
            },
            "capabilities": {},
            "workspaceFolders": [{"uri": Path(workspace).as_uri(), "name": "test"}],
        }
        return await self.send_request("initialize", params)

    async def exit(self):
        try:
            await self.shutdown()
        except Exception:
            pass
        await self.send_notification("exit")
        await self.stop()

    async def shutdown(self):
        return await self.send_request("shutdown")

    def get_abs_path(self, relative_path: str):
        return Path(self.workspace, relative_path)

    def get_file(self, relative_path: str):
        path = self.get_abs_path(relative_path)
        return self.opening_files[path]

    async def did_open(self, relative_path: str):
        path = self.get_abs_path(relative_path)

        content = ""
        with open(path, encoding="utf-8") as file:
            content = file.read()

        if path in self.opening_files:
            raise RuntimeError(f"Cannot open same file multiple times: {path}")

        self.opening_files[path] = OpeningFile(content)

        params = {
            "textDocument": {
                "uri": path.as_uri(),
                "languageId": "cpp",
                "version": 0,
                "text": content,
            }
        }

        await self.send_notification("textDocument/didOpen", params)

    async def did_change(self, relative_path: str, content: str):
        path = self.get_abs_path(relative_path)

        if path not in self.opening_files:
            raise RuntimeError(f"Cannot change closed file: {path}")

        file = self.opening_files[path]
        file.version += 1
        file.content = content
        params = {
            "textDocument": {"uri": path.as_uri(), "version": file.version},
            "contentChanges": [
                {
                    "text": content,
                }
            ],
        }

        await self.send_notification("textDocument/didChange", params)

    async def did_save(self, relative_path: str, include_text: bool = False):
        path = self.get_abs_path(relative_path)
        if path not in self.opening_files:
            raise RuntimeError(f"Cannot save closed file: {path}")

        params = {
            "textDocument": {"uri": path.as_uri()},
        }
        if include_text:
            params["text"] = self.opening_files[path].content

        await self.send_notification("textDocument/didSave", params)

    async def did_close(self, relative_path: str):
        path = self.get_abs_path(relative_path)
        if path not in self.opening_files:
            raise RuntimeError(f"Cannot close unopened file: {path}")

        del self.opening_files[path]
        params = {
            "textDocument": {"uri": path.as_uri()},
        }
        await self.send_notification("textDocument/didClose", params)

    async def hover(self, relative_path: str, line: int, character: int):
        path = self.get_abs_path(relative_path)
        params = {
            "textDocument": {"uri": path.as_uri()},
            "position": {
                "line": line,
                "character": character,
            },
        }
        return await self.send_request("textDocument/hover", params)

    async def completion(self, relative_path: str, line: int, character: int):
        path = self.get_abs_path(relative_path)
        params = {
            "textDocument": {"uri": path.as_uri()},
            "position": {
                "line": line,
                "character": character,
            },
        }
        return await self.send_request("textDocument/completion", params)

    async def signature_help(self, relative_path: str, line: int, character: int):
        path = self.get_abs_path(relative_path)
        params = {
            "textDocument": {"uri": path.as_uri()},
            "position": {
                "line": line,
                "character": character,
            },
        }
        return await self.send_request("textDocument/signatureHelp", params)
