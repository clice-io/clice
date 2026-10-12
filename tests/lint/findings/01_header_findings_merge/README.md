# Header findings reported once

A header is checked in every unit that includes it, and its findings are reported once

Each translation unit checks the headers it includes, as clang-tidy does, so a finding in a shared header comes from every unit that includes it. The report merges them into one line and lists every finding by file and position.
