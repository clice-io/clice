module;

#include "kota/async/async.h"
#include "kota/async/io/system.h"
#include "kota/codec/toml/toml.h"
#include "kota/codec/visit/common.h"
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/progress.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/text.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/ipc/peer.h"
#include "kota/ipc/protocol.h"
#include "kota/ipc/recording_transport.h"
#include "kota/ipc/transport.h"
#include "kota/meta/annotation.h"
#include "kota/meta/enum.h"
#include "kota/meta/struct.h"
#include "kota/support/ranges.h"
#include "kota/support/type_traits.h"

export module kota;

export namespace kota {

using ::kota::Formattable;
using ::kota::cancellation_source;
using ::kota::cancellation_token;
using ::kota::error;
using ::kota::event;
using ::kota::event_loop;
using ::kota::map_range;
using ::kota::mutex;
using ::kota::outcome_error;
using ::kota::pipe;
using ::kota::process;
using ::kota::queue;
using ::kota::sequence_range;
using ::kota::set_range;
using ::kota::sleep;
using ::kota::small_vector;
using ::kota::task;
using ::kota::task_group;
using ::kota::tcp;
using ::kota::timer;
using ::kota::when_all;
using ::kota::when_any;
using ::kota::with_token;
using ::kota::yield;

}  // namespace kota

export namespace kota::codec {

using ::kota::codec::RawValue;
using ::kota::codec::rich_error;

}  // namespace kota::codec

export namespace kota::codec::toml {

using ::kota::codec::toml::from_toml;
using ::kota::codec::toml::parse;

}  // namespace kota::codec::toml

export namespace kota::ipc {

using ::kota::ipc::BincodePeer;
using ::kota::ipc::Error;
using ::kota::ipc::RecordingTransport;
using ::kota::ipc::RequestResult;
using ::kota::ipc::StreamTransport;
using ::kota::ipc::Transport;
using ::kota::ipc::request_options;

}  // namespace kota::ipc

export namespace kota::ipc::lsp {

using ::kota::ipc::lsp::LineMap;
using ::kota::ipc::lsp::PositionEncoding;
using ::kota::ipc::lsp::ProgressReporter;
using ::kota::ipc::lsp::URI;
using ::kota::ipc::lsp::build_line_starts;

}  // namespace kota::ipc::lsp

export namespace kota::ipc::protocol {

using ::kota::ipc::protocol::Error;
using ::kota::ipc::protocol::ErrorCode;
using ::kota::ipc::protocol::Range;

}  // namespace kota::ipc::protocol

export namespace kota::meta {

using ::kota::meta::annotation;
using ::kota::meta::defaulted;
using ::kota::meta::enum_name;
using ::kota::meta::enum_type;
using ::kota::meta::for_each;
using ::kota::meta::reflectable_class;
using ::kota::meta::reflection;

}  // namespace kota::meta

export namespace kota::meta::attrs {

using ::kota::meta::attrs::skip;

}

export namespace kota::sys {

using ::kota::sys::memory;
using ::kota::sys::parallelism;
using ::kota::sys::priority;
using ::kota::sys::set_priority;

}  // namespace kota::sys
