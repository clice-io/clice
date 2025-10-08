#pragma once

#include "libuv.h"

#include "Task.h"
#include "Support/JSON.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/FunctionExtras.h"

namespace clice::async::net {

using Callback = llvm::unique_function<Task<void>(json::Value)>;

/// Listen on stdin/stdout, callback is called when there is a LSP message available.
void listen(Callback callback);

/// Listen on the given host and port, callback is called when there is a LSP message available.
void listen(const char* host, unsigned int port, Callback callback);

/// Write a JSON value to the client.
Task<> write(json::Value value);

}  // namespace clice::async::net
