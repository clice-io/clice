/// How completion items rank, alike wherever they come from: a compile's
/// candidates, the directives, the include paths and the module names.

module clice:feature.code_completion.ranking;

import :support.prelude;

namespace clice::feature {

/// How strongly the context asks for a candidate, apart from how its name
/// matches: clang's priority — locals before members before the rest, a
/// type the context expects ahead of all, macros last — as a factor around
/// 1, the priority of a keyword.
inline float priority_weight(unsigned priority) {
    return std::max(0.05F, (90.0F - static_cast<float>(priority)) / 50.0F);
}

/// Ascending sort text for a candidate scoring `score`, at most 8; `order`
/// breaks ties.
inline std::string sort_text(float score, std::optional<std::uint32_t> order) {
    auto text = std::format("{:.4f}", 8.0F - score);
    if(order) {
        text += std::format("{:05}", *order);
    }
    return text;
}

/// The reply listing `items` under `code_completion.limit`: the best ranked
/// stay, ties by label, and a list cut short is marked incomplete.
inline auto completion_list(std::vector<protocol::CompletionItem> items, std::uint32_t limit)
    -> protocol::CompletionList {
    protocol::CompletionList list{.is_incomplete = false, .items = std::move(items)};
    if(limit != 0 && list.items.size() > limit) {
        ranges::sort(list.items, {}, [](const protocol::CompletionItem& item) {
            return std::tie(static_cast<const std::optional<std::string>&>(item.sort_text),
                            item.label);
        });
        list.items.resize(limit);
        list.is_incomplete = true;
    }
    return list;
}

}  // namespace clice::feature
