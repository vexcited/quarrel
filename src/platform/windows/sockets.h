#ifndef QUARREL_WINDOWS_SOCKETS_H
#define QUARREL_WINDOWS_SOCKETS_H

#include <io.h>
#include <stdarg.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>

/* ========================================================================== */
/* the posix calls voice.c makes on its socket and on the pipe that wakes its */
/* thread, as winsock has them. windows polls nothing but sockets: the pipe   */
/* is a pair of them on the loopback. voice.c is the one file that includes   */
/* this, and every descriptor it closes, reads or writes is a socket.         */
/* winsock's handles fit in an int, as windows keeps them in 32 bits.         */
/* ========================================================================== */

// a socket that is not inherited, and that a datagram refused on the way
// does not make fail.
int quarrel_socket(int family, int type, int protocol);
int quarrel_pipe(int fds[2]);

typedef uint32_t in_addr_t;

#define socket quarrel_socket
#define pipe quarrel_pipe

#define close(fd) closesocket((SOCKET)(fd))
#define read(fd, data, len) recv((SOCKET)(fd), (char *)(data), (int)(len), 0)
#define write(fd, data, len) \
  send((SOCKET)(fd), (const char *)(data), (int)(len), 0)

#define send(fd, data, len, flags) \
  send((SOCKET)(fd), (const char *)(data), (int)(len), flags)
#define recv(fd, data, len, flags) \
  recv((SOCKET)(fd), (char *)(data), (int)(len), flags)
#define connect(fd, address, len) connect((SOCKET)(fd), address, (int)(len))
#define setsockopt(fd, level, name, value, len) \
  setsockopt((SOCKET)(fd), level, name, (const char *)(value), (int)(len))

#define poll(fds, n, ms) WSAPoll(fds, (ULONG)(n), ms)

// what voice.c asks of fcntl, which is to make a socket not block.
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 0x4000
#define fcntl(fd, ...) quarrel_fcntl((SOCKET)(fd), __VA_ARGS__)

static inline int quarrel_fcntl(SOCKET fd, int command, ...) {
  if (command != F_SETFL) {
    return 0;
  }

  va_list args;

  va_start(args, command);

  u_long on = (va_arg(args, int) & O_NONBLOCK) != 0;

  va_end(args);
  return ioctlsocket(fd, FIONBIO, &on) == 0 ? 0 : -1;
}

#endif
