module;

#include "kota/deco/deco.h"

export module clice:driver;

export namespace clice::driver {

void add_serve(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path);
void add_query(kota::deco::cli::SubCommander& root, int& exit_code);
void add_worker(kota::deco::cli::SubCommander& root, int& exit_code);
void add_index(kota::deco::cli::SubCommander& root, int& exit_code);
void add_doc(kota::deco::cli::SubCommander& root, int& exit_code);
void add_lint(kota::deco::cli::SubCommander& root, int& exit_code);
void add_format(kota::deco::cli::SubCommander& root, int& exit_code);

bool apply_log_level(const std::string& level_str);

template <typename Command>
void print_usage(Command& cmd) {
    std::ostringstream ss;
    cmd.usage(ss);
    std::print("{}", ss.str());
}

}  // namespace clice::driver
