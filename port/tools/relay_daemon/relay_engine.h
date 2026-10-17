#pragma once

#include "session_manager.h"
#include <atomic>

namespace relay {

bool ConfigureUdpSocket(RelaySocket socket);
void RunRelayWorker(RelaySocket socket, SessionManager& sessions, const std::atomic<bool>& running);

} // namespace relay
