#pragma once
#include "session_manager.h"
#include <atomic>
#include <string>

namespace relay {
void RunHttpApi(const std::string& bindAddress, uint16_t port, const std::string& adminToken,
                SessionManager& sessions, const std::atomic<bool>& running);
}
