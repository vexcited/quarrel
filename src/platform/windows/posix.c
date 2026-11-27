#include <winsock2.h>
#include <mstcpip.h>
#include <stdlib.h>

#include "sockets.h"
#include "win.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

int quarrel_rename(const char *from, const char *to) {
  wchar_t *a = win_wide(from), *b = win_wide(to);
  bool moved = a != nullptr && b != nullptr &&
               MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);

  free(a);
  free(b);
  return moved ? 0 : -1;
}

char *quarrel_strcasestr(const char *hay, const char *needle) {
  size_t n = strlen(needle);

  for (; *hay; hay++) {
    if (_strnicmp(hay, needle, n) == 0) {
      return (char *)hay;
    }
  }

  return n == 0 ? (char *)hay : nullptr;
}

#undef socket

int quarrel_socket(int family, int type, int protocol) {
  SOCKET s = WSASocketW(family, type, protocol, nullptr, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);

  // a port the peer closed would fail the next read, where posix drops it.
  if (s != INVALID_SOCKET && type == SOCK_DGRAM) {
    BOOL report = FALSE;
    DWORD unused = 0;

    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &unused,
             nullptr, nullptr);
  }

  return s != INVALID_SOCKET ? (int)s : -1;
}

int quarrel_pipe(int fds[2]) {
  int in = quarrel_socket(AF_INET, SOCK_DGRAM, 0);
  int out = quarrel_socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in at = {
    .sin_family = AF_INET,
    .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  struct sockaddr_in from = at;
  int len = sizeof at, from_len = sizeof from;

  // each end takes what comes from the other alone.
  if (in < 0 || out < 0 || bind((SOCKET)in, (struct sockaddr *)&at, len) != 0 ||
      getsockname((SOCKET)in, (struct sockaddr *)&at, &len) != 0 ||
      connect((SOCKET)out, (struct sockaddr *)&at, len) != 0 ||
      getsockname((SOCKET)out, (struct sockaddr *)&from, &from_len) != 0 ||
      connect((SOCKET)in, (struct sockaddr *)&from, from_len) != 0) {
    if (in >= 0) {
      closesocket((SOCKET)in);
    }

    if (out >= 0) {
      closesocket((SOCKET)out);
    }

    return -1;
  }

  fds[0] = in;
  fds[1] = out;
  return 0;
}
