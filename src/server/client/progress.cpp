module clice;

import :feature.protocol;
import :sched.index.pump;
import :server.client;
import :server.master;
import :vfs.path;

namespace clice {

void LSPClient::report_index_progress() {
    auto& p = server.index_progress;
    using Stage = IndexPump::Progress::Stage;
    auto& st = *index_progress;
    switch(p.stage) {
        case Stage::Begin: {
            st.round_active = true;
            st.total = static_cast<std::uint32_t>(p.total);
            // Progress-token creation is a server→client request, forbidden
            // until the initialize response; a round that begins before the
            // handshake stays unannounced (the next round announces).
            if(!client_ready) {
                break;
            }
            // Register a fresh work-done token; once the client acknowledges
            // it, announce the round. This is the create()+begin() handshake
            // the indexer used to run inline, now driven from the transport so
            // the indexer no longer needs a peer. The dispatch it once gated on
            // proceeds independently; reports before the token is announced are
            // dropped, so the first sub-second of a round may report fewer
            // increments than before. If a previous round's handshake is still
            // in flight, the token is reused: its continuation reconciles
            // against the current round below, so at most one create() is ever
            // outstanding and the reporter is never replaced mid-await.
            if(st.create_inflight || st.token_active) {
                break;
            }
            st.reporter.emplace(peer,
                                protocol::ProgressToken(std::string("clice/backgroundIndex")));
            st.create_inflight = true;
            // Captureless lambda with the state as a parameter: parameters
            // are copied into the coroutine frame, while captures would live
            // in the closure temporary that dies with this statement.
            //
            // Connection teardown mid-handshake needs no cancellation
            // handle. The only suspension point touching the peer is
            // create()'s send_request, and the peer fails every pending
            // outgoing request before its run() returns — that resumption
            // reads only the request's shared completion state, never the
            // peer. Both composition sites destroy the LSPClient before the
            // peer, so once the peer can be gone `abandoned` is already
            // set and the continuation below drops the token without
            // another peer access. The timeout path does reach back into
            // peer internals, but it can only fire while the peer is alive:
            // teardown completes the wait through the failure path first,
            // which also stops the timeout timer.
            server.loop.schedule([](std::shared_ptr<IndexProgressState> state) -> async::task<> {
                // Hang detection: the bound ends the handshake of a client
                // that never responds; a short one drops the progress of a
                // round whose client is merely busy.
                auto create_result =
                    co_await state->reporter->create({.timeout = std::chrono::minutes(1)});
                state->create_inflight = false;
                // The whole connection may have been torn down while the
                // handshake was in flight; drop the token without announcing it.
                if(state->abandoned || create_result.has_error()) {
                    state->reporter.reset();
                    co_return;
                }
                // The client keeps a created token until it ends, and refuses
                // to create it again meanwhile: a round that ended during the
                // handshake ends it, or no later round could show.
                if(!state->round_active) {
                    state->reporter->begin("Indexing");
                    state->reporter->end();
                    state->reporter.reset();
                    co_return;
                }
                state->reporter->begin("Indexing", std::format("0/{} files", state->total), 0);
                state->token_active = true;
            }(index_progress));
            break;
        }
        case Stage::Report: {
            if(st.token_active) {
                auto pct =
                    p.total > 0 ? static_cast<std::uint32_t>(p.completed * 100 / p.total) : 100;
                st.reporter->report(std::format("{}/{} files", p.completed, p.total), pct);
            }
            break;
        }
        case Stage::End: {
            st.round_active = false;
            if(st.token_active) {
                st.reporter->end(std::format("Indexed {} files", p.dispatched));
                st.reporter.reset();
                st.token_active = false;
            }
            // With a handshake still in flight, its continuation sees the
            // round is gone and drops the token.
            break;
        }
    }
}

/// What a compile in `phase` waits on, as its progress says.
static std::string_view describe(CompilePhase phase) {
    switch(phase) {
        case CompilePhase::Command: return "finding its command";
        case CompilePhase::Modules: return "building modules";
        case CompilePhase::Preamble: return "building preamble";
        case CompilePhase::Parsing: return "parsing";
        case CompilePhase::Idle: break;
    }
    std::unreachable();
}

void LSPClient::report_compile_phase(Fid path_id, CompilePhase phase) {
    if(!work_done_progress || !client_ready) {
        return;
    }
    auto [it, inserted] = compile_progress.try_emplace(path_id);
    if(inserted) {
        if(phase == CompilePhase::Idle) {
            compile_progress.erase(it);
            return;
        }
        it->second = std::make_shared<CompileProgress>(CompileProgress{.phase = phase});
        server.loop.schedule(announce_compile(it->second, path_id));
        return;
    }
    auto& state = *it->second;
    state.phase = phase;
    // Before the announcement, its begin() reads the phase.
    if(!state.reporter) {
        return;
    }
    if(phase == CompilePhase::Idle) {
        state.reporter->end();
        compile_progress.erase(it);
    } else {
        state.reporter->report(std::string(describe(phase)));
    }
}

async::task<> LSPClient::announce_compile(std::shared_ptr<CompileProgress> state, Fid path_id) {
    // Time is the feature: the compile after an edit mostly finishes
    // sooner, and a progress shown for each would blink in the editor.
    co_await async::sleep(std::chrono::milliseconds(500));
    if(state->abandoned) {
        co_return;
    }
    if(state->phase == CompilePhase::Idle) {
        compile_progress.erase(path_id);
        co_return;
    }
    auto shown = server.files.display(path_id);
    lsp::ProgressReporter<ipc::JSONPeer> reporter(
        peer,
        protocol::ProgressToken(std::format("clice/compile/{}", feature::to_uri(shown))));
    // Hang detection: the bound ends the handshake of a client that never
    // answers, as the index progress's does.
    auto created = co_await reporter.create({.timeout = std::chrono::minutes(1)});
    if(state->abandoned) {
        co_return;
    }
    if(created.has_error()) {
        compile_progress.erase(path_id);
        co_return;
    }
    // The client keeps a created token until it ends, and refuses to create
    // it again meanwhile: a compile that ended during the handshake ends it.
    if(state->phase == CompilePhase::Idle) {
        reporter.begin(std::string(path::filename(shown)));
        reporter.end();
        compile_progress.erase(path_id);
        co_return;
    }
    reporter.begin(std::string(path::filename(shown)), std::string(describe(state->phase)));
    state->reporter.emplace(std::move(reporter));
}

}  // namespace clice
