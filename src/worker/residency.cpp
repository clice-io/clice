module clice;

import :worker.residency;

namespace clice {

std::optional<Residency::Assignment> Residency::assignment(std::uint32_t document) const {
    auto it = entries.find(document);
    if(it == entries.end()) {
        return std::nullopt;
    }
    return it->second.assignment;
}

Residency::Assignment Residency::assign(std::uint32_t document,
                                        std::size_t worker,
                                        llvm::StringRef path) {
    assert(!entries.contains(document) && "a document assigned twice");
    Assignment assignment{.worker = worker, .id = next_assignment};
    next_assignment += 1;
    entries[document] = {.assignment = assignment, .path = path.str()};
    return assignment;
}

std::size_t Residency::documents_on(std::size_t worker) const {
    return ranges::count_if(entries, [&](const auto& entry) {
        return entry.second.assignment.worker == worker;
    });
}

void Residency::used(std::uint32_t document) {
    if(auto it = entries.find(document); it != entries.end()) {
        clock += 1;
        it->second.last_use = clock;
    }
}

void Residency::compiled(std::uint32_t document, std::uint64_t assignment, bool ast) {
    if(auto it = entries.find(document);
       it != entries.end() && it->second.assignment.id == assignment) {
        it->second.ast = ast;
    }
}

bool Residency::resident(std::uint32_t document) const {
    auto it = entries.find(document);
    return it != entries.end() && it->second.ast;
}

void Residency::hold(std::uint32_t document) {
    holds[document] += 1;
}

void Residency::release(std::uint32_t document) {
    auto it = holds.find(document);
    assert(it != holds.end() && "a document released more often than held");
    it->second -= 1;
    if(it->second == 0) {
        holds.erase(it);
    }
}

std::size_t Residency::worker_died(std::size_t worker) {
    llvm::SmallVector<std::uint32_t> lost;
    for(auto& [document, entry]: entries) {
        if(entry.assignment.worker == worker) {
            lost.push_back(document);
        }
    }
    for(auto document: lost) {
        entries.erase(document);
    }
    return lost.size();
}

std::optional<Residency::Eviction> Residency::forget(std::uint32_t document) {
    auto it = entries.find(document);
    if(it == entries.end()) {
        return std::nullopt;
    }
    Eviction eviction{.worker = it->second.assignment.worker, .path = std::move(it->second.path)};
    entries.erase(it);
    return eviction;
}

llvm::SmallVector<Residency::Eviction> Residency::evictions() {
    llvm::SmallVector<Eviction> evicted;
    auto compiled = static_cast<std::size_t>(
        ranges::count_if(entries, [](const auto& entry) { return entry.second.ast; }));
    while(compiled > limit) {
        auto victim = entries.end();
        for(auto it = entries.begin(); it != entries.end(); ++it) {
            auto& entry = it->second;
            if(entry.ast && !holds.contains(it->first) &&
               (victim == entries.end() || entry.last_use < victim->second.last_use)) {
                victim = it;
            }
        }
        if(victim == entries.end()) {
            break;
        }
        evicted.push_back(
            {.worker = victim->second.assignment.worker, .path = std::move(victim->second.path)});
        entries.erase(victim);
        compiled -= 1;
    }
    return evicted;
}

}  // namespace clice
