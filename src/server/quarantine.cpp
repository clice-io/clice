#include "server/quarantine.h"

#include <utility>

namespace clice {

void Quarantine::on_crash(std::uint8_t kind,
                          llvm::StringRef death,
                          std::string cause,
                          Clock::time_point now) {
    if(counted(death)) {
        return;
    }
    crashes += 1;
    auto& record = records[kind];
    record.strikes += 1;
    record.crash = crashes;
    record.cause = std::move(cause);
    record.last_crash = now;
    record.changed = false;
    record.saved = false;
    record.visible = record.visible || record.strikes > 1 || now - last_change >= editing_window;
}

void Quarantine::on_land(std::uint8_t kind) {
    records.erase(kind);
}

void Quarantine::on_change(Clock::time_point now) {
    last_change = now;
    changes += 1;
    for(auto& [kind, record]: records) {
        record.changed = true;
    }
}

void Quarantine::on_save() {
    for(auto& [kind, record]: records) {
        record.strikes = 0;
        record.saved = true;
    }
}

bool Quarantine::barred(std::uint8_t kind, Clock::time_point now) const {
    auto it = records.find(kind);
    if(it == records.end()) {
        return false;
    }
    auto& record = it->second;
    if(record.saved) {
        return false;
    }
    return !(record.changed && now - record.last_crash >= retry_spacing &&
             record.strikes < max_strikes);
}

Quarantine::Attempt::Attempt(Quarantine& quarantine, std::uint8_t kind) :
    quarantine(quarantine), kind(kind), changes(quarantine.changes) {
    auto it = quarantine.records.find(kind);
    if(it == quarantine.records.end()) {
        return;
    }
    auto& record = it->second;
    crash = record.crash;
    saved = std::exchange(record.saved, false);
    changed = std::exchange(record.changed, false);
}

Quarantine::Attempt::~Attempt() {
    auto it = quarantine.records.find(kind);
    if(it == quarantine.records.end()) {
        return;
    }
    auto& record = it->second;
    if(record.crash != crash) {
        record.changed = record.changed || quarantine.changes != changes;
        return;
    }
    record.saved = record.saved || saved;
    record.changed = record.changed || changed;
}

bool Quarantine::Attempt::overtaken() const {
    auto it = quarantine.records.find(kind);
    return it != quarantine.records.end() && it->second.crash != crash;
}

llvm::SmallVector<Quarantine::Note> Quarantine::notes() const {
    llvm::SmallVector<Note> notes;
    for(auto& [kind, record]: records) {
        if(record.visible) {
            notes.push_back({
                .kind = kind,
                .cause = record.cause,
                .strikes = record.strikes,
                .save_only = record.strikes >= max_strikes,
            });
        }
    }
    return notes;
}

bool Quarantine::counted(llvm::StringRef death) {
    if(death.empty()) {
        return false;
    }
    if(death == last_death) {
        return true;
    }
    last_death = death.str();
    return false;
}

}  // namespace clice
