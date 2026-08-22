#pragma once

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

#include <timeapi.h>

#include <cstddef>
#include <cstdint>

namespace hammer::net {

using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;

struct Startup {
  Startup() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    // the default 15.6ms timer tick would dominate open-loop dispatch accuracy
    timeBeginPeriod(1);
  }
  ~Startup() {
    timeEndPeriod(1);
    WSACleanup();
  }
  Startup(const Startup&) = delete;
  Startup& operator=(const Startup&) = delete;
};

inline bool valid(socket_t s) { return s != kInvalidSocket; }

inline void close_socket(socket_t s) { closesocket(s); }

inline bool set_nonblocking(socket_t s) {
  u_long mode = 1;
  return ioctlsocket(s, FIONBIO, &mode) == 0;
}

inline int last_error() { return WSAGetLastError(); }

inline bool would_block(int error) {
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

inline bool connect_in_progress(int error) {
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}

inline bool interrupted(int error) { return error == WSAEINTR; }

inline bool connection_reset(int error) {
  return error == WSAECONNRESET || error == WSAECONNABORTED;
}

inline long send_bytes(socket_t s, const char* data, size_t length) {
  const int capped = length > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(length);
  return send(s, data, capped, 0);
}

inline long recv_bytes(socket_t s, char* data, size_t length) {
  const int capped = length > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(length);
  return recv(s, data, capped, 0);
}

inline bool timed_out(int error) { return error == WSAETIMEDOUT; }

inline bool set_recv_timeout(socket_t s, int milliseconds) {
  DWORD value = static_cast<DWORD>(milliseconds);
  return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value),
                    sizeof(value)) == 0;
}

inline bool set_send_timeout(socket_t s, int milliseconds) {
  DWORD value = static_cast<DWORD>(milliseconds);
  return setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value),
                    sizeof(value)) == 0;
}

inline void ignore_sigpipe() {}

inline bool set_nodelay(socket_t s) {
  int on = 1;
  return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on),
                    sizeof(on)) == 0;
}

inline int socket_error(socket_t s) {
  int value = 0;
  int length = static_cast<int>(sizeof(value));
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &length) != 0) {
    return last_error();
  }
  return value;
}

}

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace hammer::net {

using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;

struct Startup {
  Startup() = default;
  ~Startup() = default;
  Startup(const Startup&) = delete;
  Startup& operator=(const Startup&) = delete;
};

inline bool valid(socket_t s) { return s >= 0; }

inline void close_socket(socket_t s) { ::close(s); }

inline bool set_nonblocking(socket_t s) {
  const int flags = ::fcntl(s, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline int last_error() { return errno; }

inline bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }

inline bool connect_in_progress(int error) { return error == EINPROGRESS || error == EALREADY; }

inline bool interrupted(int error) { return error == EINTR; }

inline bool connection_reset(int error) { return error == ECONNRESET || error == EPIPE; }

inline long send_bytes(socket_t s, const char* data, size_t length) {
  return static_cast<long>(::send(s, data, length, MSG_NOSIGNAL));
}

inline long recv_bytes(socket_t s, char* data, size_t length) {
  return static_cast<long>(::recv(s, data, length, 0));
}

inline bool timed_out(int error) { return error == EAGAIN || error == EWOULDBLOCK; }

inline bool set_recv_timeout(socket_t s, int milliseconds) {
  timeval value{};
  value.tv_sec = milliseconds / 1000;
  value.tv_usec = (milliseconds % 1000) * 1000;
  return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0;
}

inline bool set_send_timeout(socket_t s, int milliseconds) {
  timeval value{};
  value.tv_sec = milliseconds / 1000;
  value.tv_usec = (milliseconds % 1000) * 1000;
  return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) == 0;
}

inline void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

inline bool set_nodelay(socket_t s) {
  int on = 1;
  return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0;
}

inline int socket_error(socket_t s) {
  int value = 0;
  socklen_t length = sizeof(value);
  if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &value, &length) != 0) return last_error();
  return value;
}

}

#endif
