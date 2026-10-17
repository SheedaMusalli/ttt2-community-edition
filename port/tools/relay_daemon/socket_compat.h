#pragma once
#include <cerrno>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using RelaySocket = SOCKET;
constexpr RelaySocket kInvalidRelaySocket = INVALID_SOCKET;
inline void CloseRelaySocket(RelaySocket s) { if (s != INVALID_SOCKET) closesocket(s); }
inline bool RelayWouldBlock() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT; }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using RelaySocket = int;
constexpr RelaySocket kInvalidRelaySocket = -1;
inline void CloseRelaySocket(RelaySocket s) { if (s >= 0) ::close(s); }
inline bool RelayWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif

inline bool SetRelayNonBlocking(RelaySocket s)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(s, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}
