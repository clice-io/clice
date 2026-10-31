module clice;

import :server.session;
import :support.logging;

namespace clice {

Session::Session(Fid path_id, std::string text, int version) : path_id(path_id), version(version) {
    set_text(std::move(text));
}

void Session::set_text(std::string text) {
    source = std::make_shared<SourceText>(std::move(text));
    hash = llvm::xxh3_64bits(source->text());
}

bool Session::edit(llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
                   int version,
                   PositionEncoding encoding) {
    this->version = version;

    bool applied = false;
    for(auto& change: changes) {
        std::visit(
            [&](auto& c) {
                using T = std::remove_cvref_t<decltype(c)>;
                if constexpr(std::is_same_v<T, protocol::TextDocumentContentChangeWholeDocument>) {
                    if(text() != c.text) {
                        set_text(c.text);
                        applied = true;
                    }
                } else {
                    auto map = positions();
                    auto& range = c.range;
                    if(!map.offset(range.start, encoding) || !map.offset(range.end, encoding)) {
                        LOG_INFO(
                            "didChange range {}:{}-{}:{} does not fit the buffer "
                            "(path_id={} version={}); clamped",
                            range.start.line,
                            range.start.character,
                            range.end.line,
                            range.end.character,
                            path_id,
                            version);
                    }
                    auto [start, end] = map.offset_range(range, encoding);
                    auto current = text();
                    if(current.substr(start, end - start) != c.text) {
                        std::string edited;
                        edited.reserve(current.size() - (end - start) + c.text.size());
                        edited.append(current.take_front(start));
                        edited.append(c.text);
                        edited.append(current.drop_front(end));
                        set_text(std::move(edited));
                        applied = true;
                    }
                }
            },
            change);
    }

    generation += 1;
    if(applied) {
        this->changed(Clock::now());
    }
    return applied;
}

bool Session::crashed(feature::RequestKind kind,
                      llvm::StringRef death,
                      std::string cause,
                      Clock::time_point now) {
    auto& record = records[kind];
    bool editing = !saved_since_change && now - changed_at < policy.editing_window;
    if(record.failure.fail(policy, death, std::move(cause), now, editing)) {
        crashes += 1;
        record.seq = crashes;
    }
    return record.failure.shown;
}

bool Session::succeeded(feature::RequestKind kind, std::uint64_t generation) {
    return this->generation == generation && records.erase(kind);
}

void Session::changed(Clock::time_point now) {
    changed_at = now;
    saved_since_change = false;
    changes += 1;
    for(auto& [kind, record]: records) {
        record.failure.changed = true;
    }
}

void Session::saved() {
    saved_since_change = true;
    for(auto& [kind, record]: records) {
        record.failure.save();
    }
}

bool Session::stops(feature::RequestKind kind, Clock::time_point now) const {
    auto* record = find(kind);
    return record && record->stops(policy, now);
}

const FailureRecord* Session::find(feature::RequestKind kind) const {
    auto it = records.find(kind);
    return it != records.end() ? &it->second.failure : nullptr;
}

llvm::SmallVector<feature::CrashNote> Session::notes() const {
    llvm::SmallVector<feature::CrashNote> notes;
    for(auto& [kind, record]: records) {
        if(record.failure.shown) {
            notes.push_back({
                .kind = kind,
                .cause = record.failure.cause,
                .count = record.failure.count,
                .save_only = record.failure.save_only(policy),
            });
        }
    }
    return notes;
}

Session::Record* Session::find_record(feature::RequestKind kind) {
    auto it = records.find(kind);
    return it != records.end() ? &it->second : nullptr;
}

Session::Attempt::Attempt(Session& session, feature::RequestKind kind) :
    session(session), kind(kind), changes(session.changes) {
    if(auto* record = session.find_record(kind)) {
        auto& failure = record->failure;
        seq = record->seq;
        retrying = !failure.trying && (failure.changed || failure.saved);
        failure.trying = failure.trying || retrying;
    }
}

Session::Attempt::~Attempt() {
    auto* record = session.find_record(kind);
    if(!record) {
        return;
    }
    if(record->seq != seq) {
        record->failure.changed = record->failure.changed || session.changes != changes;
        return;
    }
    if(retrying) {
        record->failure.trying = false;
    }
}

bool Session::Attempt::crashed_meanwhile() const {
    auto* record = session.find_record(kind);
    return record && record->seq != seq;
}

}  // namespace clice
