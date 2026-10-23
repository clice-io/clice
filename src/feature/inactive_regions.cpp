module;

#include "support/prelude.h"

module clice;

import :compile.directive;
import :feature.inactive_regions;
import :support.text;
import :syntax.lexical_scan;

namespace clice::feature {

/// Where the directive at `offset` ends: past the lines a backslash
/// continues and a block comment spans.
static std::uint32_t directive_end(CompilationUnit& unit, std::uint32_t offset) {
    auto focus = unit.focus_file();
    const auto* directive = focus.lexical().directive_at(focus.content(), offset);
    assert(directive && "the preprocessor's directive is a line of the file");
    return line_end(focus.content(), directive->range.end);
}

InactiveScan inactive_regions(CompilationUnit& unit,
                              llvm::ArrayRef<std::uint8_t> open_stack,
                              std::uint32_t resume_offset,
                              std::uint32_t end_offset) {
    InactiveScan result;

    auto focus = unit.focus_file();
    auto content = focus.content();
    if(end_offset > content.size()) {
        end_offset = static_cast<std::uint32_t>(content.size());
    }

    // Walk the branch directives with an explicit nesting stack, seeded
    // from a preceding preamble scan: its pending inactive levels start
    // at the resume offset.
    struct Level {
        std::optional<std::uint32_t> inactive_begin;

        /// Whether some earlier branch of this level was taken — an #else
        /// carries no condition value, so its inactivity is derived.
        bool taken = false;
    };

    llvm::SmallVector<Level> stack;
    for(auto encoded: open_stack) {
        Level level;
        if(encoded & 1) {
            level.inactive_begin = resume_offset;
        }
        level.taken = (encoded & 2) != 0;
        stack.push_back(level);
    }

    auto is_inactive = [](const Condition& condition) {
        return condition.value == Condition::ConditionValue::False ||
               condition.value == Condition::ConditionValue::Skipped;
    };

    auto close_pending = [&](Level& level, std::uint32_t end) {
        if(!level.inactive_begin.has_value()) {
            return;
        }
        auto begin = *level.inactive_begin;
        if(begin < end) {
            result.regions.push_back(begin);
            result.regions.push_back(end);
        }
        level.inactive_begin.reset();
    };

    auto directives_it = unit.directives().find(focus.file());
    if(directives_it != unit.directives().end()) {
        for(const auto& condition: directives_it->second.conditions) {
            auto offset = unit.seen_at(condition.loc).offset;

            switch(condition.kind) {
                case Condition::BranchKind::If:
                case Condition::BranchKind::Ifdef:
                case Condition::BranchKind::Ifndef: {
                    stack.push_back({});
                    if(is_inactive(condition)) {
                        stack.back().inactive_begin = directive_end(unit, offset);
                    } else {
                        stack.back().taken = true;
                    }
                    break;
                }
                case Condition::BranchKind::Elif:
                case Condition::BranchKind::Elifdef:
                case Condition::BranchKind::Elifndef: {
                    if(stack.empty()) {
                        break;
                    }
                    close_pending(stack.back(), line_begin(content, offset));
                    if(is_inactive(condition)) {
                        stack.back().inactive_begin = directive_end(unit, offset);
                    } else {
                        stack.back().taken = true;
                    }
                    break;
                }
                case Condition::BranchKind::Else: {
                    if(stack.empty()) {
                        break;
                    }
                    close_pending(stack.back(), line_begin(content, offset));
                    // #else has no condition value: it is inactive exactly
                    // when an earlier branch of this level was taken.
                    if(stack.back().taken) {
                        stack.back().inactive_begin = directive_end(unit, offset);
                    }
                    break;
                }
                case Condition::BranchKind::EndIf: {
                    if(stack.empty()) {
                        break;
                    }
                    close_pending(stack.back(), line_begin(content, offset));
                    stack.pop_back();
                    break;
                }
            }
        }
    }

    // Levels still open at the content bound: close their regions there
    // and report the stack so a follow-up scan can resume.
    for(auto& level: stack) {
        std::uint8_t encoded = level.inactive_begin.has_value() ? 1 : 0;
        if(level.taken) {
            encoded |= 2;
        }
        result.open_stack.push_back(encoded);
        close_pending(level, end_offset);
    }

    // Nested conditionals close inner regions before the enclosing one, so
    // the list arrives unordered with inner regions contained in outer
    // ones. Canonicalize to the sorted disjoint form consumers walk with a
    // single cursor.
    auto& regions = result.regions;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    pairs.reserve(regions.size() / 2);
    for(std::size_t i = 0; i + 1 < regions.size(); i += 2) {
        pairs.emplace_back(regions[i], regions[i + 1]);
    }
    ranges::sort(pairs);
    regions.clear();
    for(auto [begin, end]: pairs) {
        if(!regions.empty() && begin <= regions.back()) {
            regions.back() = std::max(regions.back(), end);
        } else {
            regions.push_back(begin);
            regions.push_back(end);
        }
    }

    return result;
}

}  // namespace clice::feature
