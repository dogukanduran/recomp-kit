#pragma once
#include <cstdint>

void wsock_register();

namespace wsock {
// Called from user32::peek_message() to post pending WSAAsyncSelect messages
// (e.g., FD_READ, FD_WRITE, FD_ACCEPT, FD_CONNECT, FD_CLOSE) to guest HWNDs.
void pump_async_select();
} // namespace wsock
