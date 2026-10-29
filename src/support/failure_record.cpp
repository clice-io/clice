module clice;

import :support.failure_record;

namespace clice {

bool FailureRecord::fail(const FailurePolicy& policy,
                         llvm::StringRef death,
                         std::string cause,
                         Clock::time_point now,
                         bool editing) {
    if(!death.empty() && death == this->death) {
        return false;
    }
    if(now - failed_at >= policy.cooldown) {
        count = 0;
    }
    count += 1;
    failed_at = now;
    this->cause = std::move(cause);
    this->death = death.str();
    changed = false;
    saved = false;
    trying = false;
    shown = shown || count > 1 || !editing;
    return true;
}

bool FailureRecord::stops(const FailurePolicy& policy, Clock::time_point now) const {
    if(trying) {
        return true;
    }
    if(count < policy.limit || saved) {
        return false;
    }
    if(now - failed_at >= policy.cooldown) {
        return false;
    }
    return !(changed && now - failed_at >= policy.spacing && !save_only(policy));
}

}  // namespace clice
