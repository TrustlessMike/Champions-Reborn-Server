#pragma once

// Socket portability layer. On Windows this is just the Winsock headers; everywhere
// else it maps the handful of Winsock names the server uses onto BSD sockets.

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

inline int GetSocketError()
{
	return WSAGetLastError();
}

inline bool SocketWouldBlock( int err )
{
	return err == WSAEWOULDBLOCK;
}

// Winsock's fd_set is a list of handles, not a bitmap indexed by descriptor.
inline bool SocketFitsSelect( SOCKET )
{
	return true;
}

inline void SetSocketNonBlocking( SOCKET fd )
{
	u_long enabled = 1;
	ioctlsocket( fd, FIONBIO, &enabled );
}

#else

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using SOCKET = int;
using SOCKADDR = sockaddr;

constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;

inline int closesocket( SOCKET fd )
{
	return ::close( fd );
}

inline int GetSocketError()
{
	return errno;
}

inline bool SocketWouldBlock( int err )
{
	return err == EWOULDBLOCK || err == EAGAIN;
}

// fd_set is a bitmap of FD_SETSIZE descriptors; FD_SET on a larger one is out of bounds.
inline bool SocketFitsSelect( SOCKET fd )
{
	return fd >= 0 && fd < FD_SETSIZE;
}

inline void SetSocketNonBlocking( SOCKET fd )
{
	const int flags = ::fcntl( fd, F_GETFL, 0 );
	if( flags != -1 )
	{
		::fcntl( fd, F_SETFL, flags | O_NONBLOCK );
	}
}

inline int InetPtonA( int family, const char *src, void *dst )
{
	return ::inet_pton( family, src, dst );
}

inline const char *InetNtopA( int family, const void *src, char *dst, size_t size )
{
	return ::inet_ntop( family, src, dst, static_cast< socklen_t >( size ) );
}

#endif
