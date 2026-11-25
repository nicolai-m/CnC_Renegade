// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SOCKETPLATFORM_H
#define SOCKETPLATFORM_H

#if defined(__linux__)
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>

typedef int SOCKET;
typedef sockaddr_in SOCKADDR_IN;
typedef sockaddr *LPSOCKADDR;
typedef sockaddr_in *LPSOCKADDR_IN;
static const SOCKET INVALID_SOCKET = -1;
static const int SOCKET_ERROR = -1;
#else
#include "win.h"
#include <winsock.h>
#endif

#if defined(__linux__)
typedef socklen_t SocketLength;
#else
typedef int SocketLength;
#endif

inline int Last_Socket_Error()
{
#if defined(__linux__)
	return errno;
#else
	return WSAGetLastError();
#endif
}

inline bool Socket_Would_Block(int error)
{
#if defined(__linux__)
	return error == EAGAIN || error == EWOULDBLOCK;
#else
	return error == WSAEWOULDBLOCK;
#endif
}

#endif // SOCKETPLATFORM_H
