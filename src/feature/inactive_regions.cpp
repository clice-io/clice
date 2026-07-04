#include "feature/inactive_regions.h"

#include "compile/directive.h"

#include "llvm/ADT/SmallVector.h"

namespace clice::feature {

InactiveScan inactive_regions(CompilationUnitRef unit,
                              llvm::ArrayRef<std::uint8_t> open_stack,
                              std::uint32_t resume_offset,
                              std::uint32_t end_offset) {
    InactiveScan result;

    auto interested = unit.interested_file();
    auto content = unit.file_content(interested);
    if(end_offset > content.size()) {
        end_offset = static_cast<std::uint32_t>(content.size());
    }

    // Offset just past the end of the line containing `offset`.
    auto line_end = [&](std::uint32_t offset) -> std::uint32_t {
        auto pos = content.find('\n', offset);
        return pos == llvm::StringRef::npos ? static_cast<std::uint32_t>(content.size())
                                            : static_cast<std::uint32_t>(pos + 1);
    };

    // Offset of the start of the line containing `offset`.
    auto line_begin = [&](std::uint32_t offset) -> std::uint32_t {
        auto pos = content.rfind('\n', offset);
        return pos == llvm::StringRef::npos ? 0 : static_cast<std::uint32_t>(pos + 1);
    };

    auto local_offset = [&](clang::SourceLocation loc) -> std::optional<std::uint32_t> {
        auto [fid, offset] = unit.decompose_location(loc);
        if(fid != interested) {
            return std::nullopt;
        }
        return offset;
    };

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

    auto directives_it = unit.directives().find(interested);
    if(directives_it != unit.directives().end()) {
        for(const auto& condition: directives_it->second.conditions) {
            auto offset = local_offset(condition.loc);
            if(!offset) {
                continue;
            }

            switch(condition.kind) {
                case Condition::BranchKind::If:
                case Condition::BranchKind::Ifdef:
                case Condition::BranchKind::Ifndef: {
                    stack.push_back({});
                    if(is_inactive(condition)) {
                        stack.back().inactive_begin = line_end(*offset);
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
                    close_pending(stack.back(), line_begin(*offset));
                    if(is_inactive(condition)) {
                        stack.back().inactive_begin = line_end(*offset);
                    } else {
                        stack.back().taken = true;
                    }
                    break;
                }
                case Condition::BranchKind::Else: {
                    if(stack.empty()) {
                        break;
                    }
                    close_pending(stack.back(), line_begin(*offset));
                    // #else has no condition value: it is inactive exactly
                    // when an earlier branch of this level was taken.
                    if(stack.back().taken) {
                        stack.back().inactive_begin = line_end(*offset);
                    }
                    break;
                }
                case Condition::BranchKind::EndIf: {
                    if(stack.empty()) {
                        break;
                    }
                    close_pending(stack.back(), line_begin(*offset));
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

    return result;
}

}  // namespace clice::feature
