#include "feature/inactive_regions.h"

#include "compile/directive.h"

#include "llvm/ADT/SmallVector.h"

namespace clice::feature {

std::vector<std::uint32_t> inactive_regions(CompilationUnitRef unit) {
    std::vector<std::uint32_t> regions;

    auto interested = unit.interested_file();
    auto directives_it = unit.directives().find(interested);
    if(directives_it == unit.directives().end()) {
        return regions;
    }

    auto content = unit.file_content(interested);

    // Offset just past the end of the line containing `offset`.
    auto line_end = [&](std::uint32_t offset) -> std::uint32_t {
        auto pos = content.find('\n', offset);
        return pos == llvm::StringRef::npos ? content.size() : static_cast<std::uint32_t>(pos + 1);
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

    // Walk the branch directives with an explicit nesting stack. Each level
    // remembers where its currently-inactive branch body started; the next
    // sibling directive (elif/else/endif) closes it.
    struct Level {
        std::optional<std::uint32_t> inactive_begin;
    };

    llvm::SmallVector<Level> stack;

    auto is_inactive = [](const Condition& condition) {
        return condition.value == Condition::ConditionValue::False ||
               condition.value == Condition::ConditionValue::Skipped;
    };

    auto close_pending = [&](Level& level, std::uint32_t terminator_offset) {
        if(!level.inactive_begin.has_value()) {
            return;
        }
        auto begin = *level.inactive_begin;
        auto end = line_begin(terminator_offset);
        if(begin < end) {
            regions.push_back(begin);
            regions.push_back(end);
        }
        level.inactive_begin.reset();
    };

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
                }
                break;
            }
            case Condition::BranchKind::Elif:
            case Condition::BranchKind::Elifdef:
            case Condition::BranchKind::Elifndef:
            case Condition::BranchKind::Else: {
                if(stack.empty()) {
                    break;
                }
                close_pending(stack.back(), *offset);
                if(is_inactive(condition)) {
                    stack.back().inactive_begin = line_end(*offset);
                }
                break;
            }
            case Condition::BranchKind::EndIf: {
                if(stack.empty()) {
                    break;
                }
                close_pending(stack.back(), *offset);
                stack.pop_back();
                break;
            }
        }
    }

    return regions;
}

}  // namespace clice::feature
