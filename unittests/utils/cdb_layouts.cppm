module;

#include "modules/prelude.h"

module clice:unittests.utils.cdb_layouts;

import :unittests.utils.temp_dir;
import :vfs.path;

namespace clice::testing {

struct LayoutFile {
    llvm::StringRef path;
    llvm::StringRef content;
};

struct NamedLayout {
    llvm::StringRef name;
    std::vector<LayoutFile> files;
};

/// Projects the database and build tests load the way a real project is
/// loaded, each pinning one property of how databases and rules combine.
const inline std::vector<NamedLayout>& cdb_layouts() {
    const static std::vector<NamedLayout> layouts = {
        {"command_string",
         {
             {"compile_commands.json", R"([
  {
    "directory": ".",
    "file": "main.cpp",
    "command": "clang++ -std=c++20 -DCMD -c main.cpp -o main.o"
  }
]
)"},
             {"main.cpp", R"(int main() {
    return 0;
}
)"},
         }},
        {"declared_ignores_discovered",
         {
             {"clice.toml", R"([[rules]]
default_command = ["clang++", "-std=c++20", "-DFROM_RULE"]
)"},
             {"compile_commands.json", R"([
  {
    "directory": ".",
    "file": "main.cpp",
    "arguments": ["clang++", "-std=c++20", "-DFROM_CDB", "main.cpp"]
  }
]
)"},
             {"main.cpp", R"(int main() {
    return 0;
}
)"},
         }},
        {"default_command_only",
         {
             {"clice.toml", R"([[rules]]
patterns = ["src/**", "include/**"]
default_command = "clang++ -std=c++20 -DDEFAULTED -Iinclude"

[[rules]]
patterns = ["src/skip/**"]
index = false
)"},
             {"include/lib.h", R"(inline int lib() {
    return 0;
}
)"},
             {"src/main.cpp", R"(#include "lib.h"

int main() {
    return lib();
}
)"},
             {"src/skip/vendored.cpp", R"(int vendored() {
    return 0;
}
)"},
             {"tools/other.cpp", R"(int other() {
    return 0;
}
)"},
         }},
        {"launcher_prefix",
         {
             {"compile_commands.json", R"([
  {
    "directory": ".",
    "file": "main.cpp",
    "arguments": ["ccache", "clang++", "-std=c++20", "-DWRAPPED", "main.cpp"]
  }
]
)"},
             {"main.cpp", R"(int main() {
    return 0;
}
)"},
         }},
        {"lint_rules",
         {
             {"clice.toml", R"([[rules]]
patterns = ["vendor/**"]
lint = false

[[rules]]
patterns = ["gen/**"]
format = false

[[rules]]
compile_commands = ["cmake"]
)"},
             {"cmake/compile_commands.json", R"([
  {
    "directory": "..",
    "file": "src/main.cpp",
    "arguments": ["clang++", "-std=c++20", "src/main.cpp"]
  },
  {
    "directory": "..",
    "file": "vendor/lib.cpp",
    "arguments": ["clang++", "-std=c++20", "vendor/lib.cpp"]
  }
]
)"},
             {"src/main.cpp", R"(int main() {
    return 0;
}
)"},
             {"vendor/lib.cpp", R"(int lib() {
    return 0;
}
)"},
         }},
        {"multi_entry",
         {
             {"compile_commands.json", R"([
  {
    "directory": ".",
    "file": "main.cpp",
    "output": "out/Debug/main.o",
    "arguments": ["clang++", "-std=c++20", "-DCONFIG_A", "-c", "main.cpp", "-o", "out/Debug/main.o"]
  },
  {
    "directory": ".",
    "file": "main.cpp",
    "output": "out/Release/main.o",
    "arguments": [
      "clang++",
      "-std=c++20",
      "-DCONFIG_B",
      "-c",
      "main.cpp",
      "-o",
      "out/Release/main.o"
    ]
  }
]
)"},
             {"main.cpp", R"(int main() {
    return 0;
}
)"},
         }},
        {"relative_directory",
         {
             {"compile_commands.json", R"([
  {
    "directory": "src",
    "file": "main.cpp",
    "arguments": ["clang++", "-std=c++20", "-DRELATIVE", "main.cpp"]
  }
]
)"},
             {"src/main.cpp", R"(int main() {
    return 0;
}
)"},
         }},
        {"response_file",
         {
             {"compile_commands.json", R"([
  {
    "directory": ".",
    "file": "main.cpp",
    "arguments": ["clang++", "@flags.rsp", "main.cpp"]
  }
]
)"},
             {"flags.rsp", R"(-std=c++20
-DFROM_RSP
)"},
             {"main.cpp", R"(int main() {
    return 0;
}
)"},
         }},
        {"rules_bound",
         {
             {"clice.toml", R"([[rules]]
patterns = ["lib/**"]
compile_commands = ["lib/cmake"]

[[rules]]
patterns = ["**/*.hxx"]
append = ["-x", "c++-header"]

[[rules]]
compile_commands = ["cmake"]
)"},
             {"cmake/compile_commands.json", R"([
  {
    "directory": "..",
    "file": "src/a.cpp",
    "arguments": ["clang++", "-std=c++20", "-DROOT", "src/a.cpp"]
  },
  {
    "directory": "..",
    "file": "lib/x.cpp",
    "arguments": ["clang++", "-std=c++20", "-DROOT", "lib/x.cpp"]
  }
]
)"},
             {"lib/cmake/compile_commands.json", R"([
  {
    "directory": "..",
    "file": "x.cpp",
    "arguments": ["clang++", "-std=c++20", "-DLIB", "x.cpp"]
  }
]
)"},
             {"lib/x.cpp", R"(int x() {
    return 0;
}
)"},
             {"lib/y.hxx", R"(inline int y() { return 0; }
)"},
             {"src/a.cpp", R"(int main() {
    return 0;
}
)"},
         }},
        {"single_root",
         {
             {"compile_commands.json", R"([
  {
    "directory": ".",
    "file": "main.cpp",
    "arguments": ["clang++", "-std=c++20", "-DSINGLE", "main.cpp"]
  }
]
)"},
             {"main.cpp", R"(int main() {
    return 0;
}
)"},
         }},
        {"subdir_cdb",
         {
             {"cmake/compile_commands.json", R"([
  {
    "directory": ".",
    "file": "../main.cpp",
    "arguments": ["clang++", "-std=c++20", "-DOUT", "../main.cpp"]
  }
]
)"},
             {"main.cpp", R"(int main() {
    return 0;
}
)"},
         }},
    };
    return layouts;
}

/// Writes layout `name` into `dir/name` and returns that directory.
inline std::string write_layout(TempDir& dir, llvm::StringRef name) {
    auto found = std::ranges::find(cdb_layouts(), name, &NamedLayout::name);
    assert(found != cdb_layouts().end() && "no such layout");
    for(auto& file: found->files) {
        dir.touch(path::join(name, file.path), file.content);
    }
    return dir.path(name);
}

}  // namespace clice::testing
