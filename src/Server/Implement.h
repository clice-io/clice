#pragma once

#include "Server/Plugin.h"
#include "Server/Server.h"

namespace clice {

struct ServerRef::Self {
public:
    Self(Server* server_instance) : server_instance(server_instance) {}

    Server& server() const {
        return *server_instance;
    }

    Server* server_instance;
};

}  // namespace clice
