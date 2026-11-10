/// A compiler driver of the toolchain tests on Windows, which runs no shell
/// script by itself: copied in under a driver's name, it runs the script
/// beside it, named as it is with .sh for .exe, in the sh on PATH (Git for
/// Windows'), with its own arguments, and exits as the script does.

#include <cstdio>
#include <string>
#include <string_view>
#include <windows.h>

int main() {
    std::wstring self(MAX_PATH, L'\0');
    while(true) {
        auto length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if(length < self.size()) {
            self.resize(length);
            break;
        }
        self.resize(self.size() * 2);
    }
    auto script = self.substr(0, self.rfind(L'.')) + L".sh";

    // The arguments as the caller quoted them: the program name ends at its
    // closing quote, or at the first blank when it has none.
    std::wstring_view line = GetCommandLineW();
    auto end = line.starts_with(L'"') ? line.find(L'"', 1) + 1 : line.find_first_of(L" \t");
    auto arguments = end > line.size() ? std::wstring_view() : line.substr(end);

    std::wstring command = L"sh \"" + script + L"\"" + std::wstring(arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,
                       command.data(),
                       nullptr,
                       nullptr,
                       TRUE,
                       0,
                       nullptr,
                       nullptr,
                       &startup,
                       &process)) {
        std::fprintf(stderr,
                     "fake_driver: cannot start sh (error %lu): the toolchain tests run their "
                     "compilers' scripts in the sh of Git for Windows, on PATH\n",
                     GetLastError());
        return 127;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    return static_cast<int>(code);
}
