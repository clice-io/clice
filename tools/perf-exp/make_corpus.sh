#!/usr/bin/env bash
# Build a frozen, platform-independent corpus: pinned sources preprocessed on
# Linux against the runner's libstdc++/glibc, so every platform parses
# byte-identical input with --target=x86_64-unknown-linux-gnu.
set -euo pipefail
REF="$1"   # reference clang (llvm-mingw)
OUT="$2"
mkdir -p "$OUT" src
cd src

curl -fsSLO https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
unzip -q -o sqlite-amalgamation-3460100.zip
curl -fsSL -o json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
git clone -q --depth 1 --branch v24.3.25 https://github.com/google/flatbuffers.git

cat > json_use.cpp <<'EOF'
#include "json.hpp"
#include <map>
#include <string>
#include <vector>
struct Point { double x, y; std::string tag; std::vector<int> ids; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Point, x, y, tag, ids)
int main() {
    nlohmann::json j = nlohmann::json::parse(R"({"a":[1,2,3],"b":{"c":"d"}})");
    std::map<std::string, std::vector<Point>> m = {{"k", {{1, 2, "t", {1, 2}}}}};
    nlohmann::json k = m;
    auto back = k.get<std::map<std::string, std::vector<Point>>>();
    return static_cast<int>(j.dump().size() + back.size());
}
EOF

cat > std_heavy.cpp <<'EOF'
#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>
int main() {
    std::vector<int> v{5, 3, 1, 4};
    auto r = v | std::views::filter([](int x) { return x % 2; }) | std::views::transform([](int x) { return x * 2; });
    std::map<std::string, std::variant<int, std::string>> m{{"a", 1}, {"b", std::string("x")}};
    std::regex re("[a-z]+");
    std::unordered_map<int, std::optional<std::tuple<int, double>>> u;
    u[1] = std::tuple{1, 2.0};
    auto s = std::format("{} {}", v.size(), m.size());
    std::ranges::sort(v);
    return static_cast<int>(std::ranges::distance(r) + s.size() + std::regex_match("abc", re));
}
EOF

T=--target=x86_64-unknown-linux-gnu
"$REF" $T -std=c11 -E -DSQLITE_THREADSAFE=0 sqlite-amalgamation-3460100/sqlite3.c -o "$OUT/sqlite3.c"
"$REF" $T -std=c++20 -E json_use.cpp -o "$OUT/json_use.cpp"
"$REF" $T -std=c++23 -E std_heavy.cpp -o "$OUT/std_heavy.cpp"
for f in idl_parser reflection; do
    "$REF" $T -std=c++17 -E -Iflatbuffers/include flatbuffers/src/$f.cpp -o "$OUT/$f.cpp"
done
cat > "$OUT/manifest.json" <<'EOF2'
{"sqlite3.c": "-std=c11", "json_use.cpp": "-std=c++20", "std_heavy.cpp": "-std=c++23",
 "idl_parser.cpp": "-std=c++17", "reflection.cpp": "-std=c++17"}
EOF2
ls -la "$OUT"
