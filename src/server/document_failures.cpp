module clice;

import :server.document_failures;

namespace clice {

bool DocumentFailures::crashed(Fid document,
                               feature::RequestKind kind,
                               llvm::StringRef death,
                               std::string cause,
                               Clock::time_point now) {
    auto& entry = documents[document];
    auto& record = entry.records[kind];
    bool editing = !entry.saved_since_change && now - entry.changed_at < policy.editing_window;
    if(record.failure.fail(policy, death, std::move(cause), now, editing)) {
        crashes += 1;
        record.serial = crashes;
    }
    return record.failure.shown;
}

bool DocumentFailures::succeeded(const Ticket& ticket, feature::RequestKind kind) {
    auto it = documents.find(ticket.session->path_id);
    return ticket.fresh() && it != documents.end() && it->second.records.erase(kind);
}

void DocumentFailures::changed(Fid document, Clock::time_point now) {
    auto& entry = documents[document];
    entry.changed_at = now;
    entry.saved_since_change = false;
    entry.changes += 1;
    for(auto& [kind, record]: entry.records) {
        record.failure.changed = true;
    }
}

void DocumentFailures::saved(Fid document) {
    auto& entry = documents[document];
    entry.saved_since_change = true;
    for(auto& [kind, record]: entry.records) {
        record.failure.save();
    }
}

void DocumentFailures::closed(Fid document, std::uint64_t hash) {
    documents[document].closed_on = hash;
}

void DocumentFailures::opened(Fid document, std::uint64_t hash, Clock::time_point now) {
    auto it = documents.find(document);
    if(it == documents.end()) {
        return;
    }
    // FIXME: only the document's own text is compared: a header it includes,
    // fixed while it was closed, leaves its crashed kinds stopped until an edit
    // or save, though the crash note says a header change lifts them.
    if(auto closed_on = std::exchange(it->second.closed_on, std::nullopt);
       closed_on && *closed_on != hash) {
        changed(document, now);
    }
}

bool DocumentFailures::stops(Fid document, feature::RequestKind kind, Clock::time_point now) const {
    auto* record = find(document, kind);
    return record && record->stops(policy, now);
}

const FailureRecord* DocumentFailures::find(Fid document, feature::RequestKind kind) const {
    auto it = documents.find(document);
    if(it == documents.end()) {
        return nullptr;
    }
    auto record = it->second.records.find(kind);
    return record != it->second.records.end() ? &record->second.failure : nullptr;
}

bool DocumentFailures::any(Fid document) const {
    auto it = documents.find(document);
    return it != documents.end() && !it->second.records.empty();
}

llvm::SmallVector<feature::CrashNote> DocumentFailures::notes(Fid document) const {
    llvm::SmallVector<feature::CrashNote> notes;
    auto it = documents.find(document);
    if(it == documents.end()) {
        return notes;
    }
    for(auto& [kind, record]: it->second.records) {
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

DocumentFailures::Record* DocumentFailures::find_record(Fid document, feature::RequestKind kind) {
    auto it = documents.find(document);
    if(it == documents.end()) {
        return nullptr;
    }
    auto record = it->second.records.find(kind);
    return record != it->second.records.end() ? &record->second : nullptr;
}

DocumentFailures::Attempt::Attempt(DocumentFailures& failures,
                                   Fid document,
                                   feature::RequestKind kind) :
    failures(failures), document(document), kind(kind) {
    auto it = failures.documents.find(document);
    changes = it != failures.documents.end() ? it->second.changes : 0;
    if(auto* record = failures.find_record(document, kind)) {
        auto& failure = record->failure;
        serial = record->serial;
        retrying = !failure.trying && (failure.changed || failure.saved);
        failure.trying = failure.trying || retrying;
    }
}

DocumentFailures::Attempt::~Attempt() {
    auto* record = failures.find_record(document, kind);
    if(!record) {
        return;
    }
    if(record->serial != serial) {
        auto& entry = failures.documents.find(document)->second;
        record->failure.changed = record->failure.changed || entry.changes != changes;
        return;
    }
    if(retrying) {
        record->failure.trying = false;
    }
}

bool DocumentFailures::Attempt::crashed_meanwhile() const {
    auto* record = failures.find_record(document, kind);
    return record && record->serial != serial;
}

}  // namespace clice
