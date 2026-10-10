// ╔═╗ ╦ ╦ ╔═╗ ╔╦╗ ╔═╗ ╦ ╔═╗ ╔╗╔ ╔═╗
// ║   ╠═╣ ╠═╣ ║║║ ╠═╝ ║ ║ ║ ║║║ ╚═╗
// ╚═╝ ╩ ╩ ╩ ╩ ╩ ╩ ╩   ╩ ╚═╝ ╝╚╝ ╚═╝
// ╦  ╔═╗╔╗ ╔╗ ╦ ╦  ╔═╗╔═╗╦═╗╦  ╦╔═╗╦═╗
// ║  ║ ║╠╩╗╠╩╗╚╦╝  ╚═╗║╣ ╠╦╝╚╗╔╝║╣ ╠╦╝
// ╩═╝╚═╝╚═╝╚═╝ ╩   ╚═╝╚═╝╩╚═ ╚╝ ╚═╝╩╚═

#include "Lobby Server/LobbyServer.hpp"

#include <algorithm>

#include "Game/RealmUserManager.hpp"
#include "Network/Events.hpp"
#include "configuration.hpp"
#include "logging.hpp"

LobbyServer::LobbyServer()
{
	m_running = false;
	m_conSocket.reset();
	m_rtaSocket.reset();

	m_gatewaySockets.clear();

	m_clientSockets.clear();
	m_recvBuffer.resize( 1024 );
}

LobbyServer::~LobbyServer()
{
	Log::Info( "Lobby Server stopped." );

	for( auto &socket : m_clientSockets )
	{
		if( socket->fd != INVALID_SOCKET )
		{
			closesocket( socket->fd );
		}
	}

	for( auto &socket : m_gatewaySockets )
	{
		if( socket->fd != INVALID_SOCKET )
		{
			closesocket( socket->fd );
		}
	}
}

void LobbyServer::Start( std::string ip )
{
	// Champions of Norrath Gateway Sockets
	for( int i = 0; i < 8; i++ )
	{
		auto gatewaySocket = OpenListenerSocket( ip, 40800 + i, RealmGameType::CHAMPIONS_OF_NORRATH );
		if( nullptr == gatewaySocket ) return;

		gatewaySocket->flag.is_gateway = true;
		m_gatewaySockets.push_back( gatewaySocket );
	}

	// Return to Arms Gateway Sockets
	for( int i = 0; i < 8; i++ )
	{
		auto gatewaySocket = OpenListenerSocket( ip, 40810 + i, RealmGameType::RETURN_TO_ARMS );
		if( nullptr == gatewaySocket ) return;

		gatewaySocket->flag.is_gateway = true;
		m_gatewaySockets.push_back( gatewaySocket );
	}

	m_conSocket = OpenListenerSocket( ip, Config::con_lobby_port, RealmGameType::CHAMPIONS_OF_NORRATH );
	m_rtaSocket = OpenListenerSocket( ip, Config::rta_lobby_port, RealmGameType::RETURN_TO_ARMS );

	m_running = true;
	m_thread = std::thread( &LobbyServer::Run, this );

	Log::Info( "Lobby Server started" );
}

void LobbyServer::Stop()
{
	Log::Info( "Stopping Lobby Server..." );

	{
		// The lobby thread is still running and owns this list.
		std::lock_guard< std::mutex > stateLock( m_stateMutex );

		for( auto &client : m_clientSockets )
		{
			client->send( NotifyForcedLogout() );
		}
	}

	m_running = false;
	if( m_thread.joinable() )
	{
		m_thread.join();
	}
}

sptr_socket LobbyServer::OpenListenerSocket( std::string ip, int32_t port, RealmGameType type )
{
#ifdef _WIN32
	SOCKET sock = ::WSASocket( AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED );
#else
	SOCKET sock = ::socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
#endif
	if( sock == INVALID_SOCKET )
	{
		Log::Error( "socket() failed on port {}", port );
		return nullptr;
	}

#ifndef _WIN32
	// Without this a restart fails to bind while old connections sit in TIME_WAIT.
	int reuse = 1;
	setsockopt( sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
#endif

	sockaddr_in service{};
	service.sin_family = AF_INET;
	service.sin_port = htons( port );

	if( ip == "0.0.0.0" )
	{
		service.sin_addr.s_addr = INADDR_ANY;
	}
	else if( InetPtonA( AF_INET, ip.c_str(), &service.sin_addr) != 1 )
	{
		Log::Error( "Invalid IP address format: {}", ip );
		closesocket( sock );
		return nullptr;
	}

	if( bind( sock, reinterpret_cast< SOCKADDR * >( &service ), sizeof( service ) ) == SOCKET_ERROR )
	{
		Log::Error( "bind() failed on port {}", port );
		closesocket( sock );
		return nullptr;
	}

	if( listen( sock, SOMAXCONN ) == SOCKET_ERROR )
	{
		Log::Error( "listen() failed on port {}", port );
		closesocket( sock );
		return nullptr;
	}

	auto realmSocket = std::make_shared< RealmSocket >();
	realmSocket->fd = sock;
	realmSocket->remote_ip = ip;
	realmSocket->remote_port = port;
	realmSocket->gameType = type;
	realmSocket->flag.is_listener = true;

	Log::Info( "Socket Opened on {}:{}", ip, port );

	return realmSocket;
}

void LobbyServer::Run()
{
	fd_set readSet;
	fd_set writeSet;

	while( m_running )
	{
		// select() may modify the timeout, and outside Windows it needs the highest fd.
		timeval timeout = { 0, 1000 };
		SOCKET maxFd = 0;

		FD_ZERO( &readSet );
		FD_ZERO( &writeSet );

		FD_SET( m_conSocket->fd, &readSet );
		FD_SET( m_rtaSocket->fd, &readSet );
		maxFd = std::max( { maxFd, m_conSocket->fd, m_rtaSocket->fd } );

		for( const auto &sock : m_gatewaySockets )
		{
			if( sock != nullptr && sock->fd != INVALID_SOCKET )
			{
				FD_SET( sock->fd, &readSet );
				maxFd = std::max( maxFd, sock->fd );
			}
		}

		{
			std::lock_guard< std::mutex > stateLock( m_stateMutex );
			CheckSocketProblem();
		}

		for( const auto &client : m_clientSockets )
		{
			FD_SET( client->fd, &readSet );
			maxFd = std::max( maxFd, client->fd );

			// Only ask for writability when there is something to send; an idle
			// socket is always writable and would turn this loop into a busy spin.
			std::lock_guard< std::mutex > lock( client->write_mutex );
			if( !client->m_pendingWriteBuffer.empty() )
			{
				FD_SET( client->fd, &writeSet );
			}
		}

		auto result = select( static_cast< int >( maxFd ) + 1, &readSet, &writeSet, NULL, &timeout );

		if( result == SOCKET_ERROR )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			continue;
		}

		// Users, games and rooms are also reached from the discovery thread, so
		// everything below runs with the state lock held.
		std::lock_guard< std::mutex > stateLock( m_stateMutex );

		// Nothing a client sends may take the server down. Requests are already
		// guarded one by one in HandleRequest; this is the last line of defence.
		try
		{
			for( auto &sock : m_gatewaySockets )
			{
				if( sock == nullptr || sock->fd == INVALID_SOCKET )
					continue;

				if( FD_ISSET( sock->fd, &readSet ) )
				{
					AcceptConnection( sock );
				}
			}

			if( FD_ISSET( m_conSocket->fd, &readSet ) )
			{
				AcceptConnection( m_conSocket );
			}

			if( FD_ISSET( m_rtaSocket->fd, &readSet ) )
			{
				AcceptConnection( m_rtaSocket );
			}

			for( auto &client : m_clientSockets )
			{
				if( FD_ISSET( client->fd, &readSet ) )
				{
					ReadSocket( client );
				}

				if( FD_ISSET( client->fd, &writeSet ) )
				{
					WriteSocket( client );
				}
			}
		}
		catch( const std::exception &e )
		{
			Log::Error( "[LOBBY] Unhandled exception: {}", std::string( e.what() ) );
		}
		catch( ... )
		{
			Log::Error( "[LOBBY] Unhandled exception of unknown type" );
		}
	}
}

void LobbyServer::CheckSocketProblem()
{
	auto now = std::chrono::steady_clock::now();

	for( auto it = m_clientSockets.begin(); it != m_clientSockets.end(); )
	{
		auto &socket = *it;
		auto elapsed = std::chrono::duration_cast< std::chrono::seconds >( now - socket->last_recv_time );

		// Check for client timeouts
		if( elapsed.count() > Config::client_timeout )
		{
			socket->flag.disconnected_forced = true;
			Log::Info( "[LOBBY] Client Timeout : ({})", socket->remote_ip );
		}

		// Check if we're waiting to disconnect after sending buffered data
		if( socket->flag.disconnected_wait && socket->m_pendingWriteBuffer.empty() )
		{
			socket->flag.disconnected_forced = true;
		}

		// Replies pile up here when the client does not read them. The largest thing
		// the server ever sends is one character, about 20 KB.
		if( socket->m_pendingWriteBuffer.size() > MAX_PENDING_WRITE_BYTES )
		{
			socket->flag.disconnected_forced = true;
			Log::Info( "[LOBBY] Client is not reading its replies : ({})", socket->remote_ip );
		}

		if( socket->flag.disconnected_forced )
		{
			UserManager::Get().RemoveUser( socket );
			Log::Info( "[LOBBY] Client Disconnected : ({})", socket->remote_ip );

			// Close here rather than in the destructor: something else may still hold
			// a reference to the socket, and the connection must not outlive this list.
			if( socket->fd != INVALID_SOCKET )
			{
				closesocket( socket->fd );
				socket->fd = INVALID_SOCKET;
			}

			it = m_clientSockets.erase( it );
		}
		else
		{
			++it;
		}
	}
}

void LobbyServer::AcceptConnection( sptr_socket srcSocket )
{
	sockaddr_in clientInfo{};
	socklen_t addrSize = sizeof( clientInfo );

	SOCKET clientSocket = accept( srcSocket->fd, ( SOCKADDR * )&clientInfo, &addrSize );
	if( clientSocket == INVALID_SOCKET )
	{
		Log::Error( "accept() failed" );
		return;
	}

	// select() cannot watch a descriptor beyond its set size, and marking one in the
	// set anyway writes outside it.
	if( !SocketFitsSelect( clientSocket ) )
	{
		Log::Error( "Too many connections, refusing a new one" );
		closesocket( clientSocket );
		return;
	}

	auto new_socket = std::make_shared< RealmSocket >();
	auto gameType = srcSocket->gameType;

	// One client that sends requests and never reads the replies must not be able to
	// make send() wait, and with it every other client: this thread serves them all.
	SetSocketNonBlocking( clientSocket );

	new_socket->fd = clientSocket;
	new_socket->remote_addr = clientInfo;
	new_socket->remote_ip = Util::IPFromAddr( clientInfo );
	new_socket->remote_port = ntohs( clientInfo.sin_port );
	new_socket->gameType = gameType;
	m_clientSockets.push_back( new_socket );

	if( !srcSocket->flag.is_gateway )
	{
		UserManager::Get().CreateUser( new_socket, gameType );
	}

	Log::Info( "New Client Connected : ({}) From Port {}", new_socket->remote_ip, srcSocket->remote_port );
}

void LobbyServer::ReadSocket( sptr_socket socket )
{
	if( socket->flag.disconnected_forced )
	{
		return;
	}

	const auto bytesReceived = recv( socket->fd, reinterpret_cast< char * >( m_recvBuffer.data() ), static_cast< int >( m_recvBuffer.size() ), 0 );

	if( bytesReceived == SOCKET_ERROR )
	{
		// Client sockets do not block, and select() can report one readable that is not.
		const int err = GetSocketError();
		if( SocketWouldBlock( err ) )
		{
			return;
		}

		Log::Info( "Socket Error [{}].", err );
		socket->flag.disconnected_forced = true;
		return;
	}

	if( bytesReceived == 0 )
	{
		socket->flag.disconnected_forced = true;
		return;
	}

	socket->last_recv_time = std::chrono::steady_clock::now();

	// Append received data to socket's pending buffer
	socket->m_pendingReadBuffer.insert(
		socket->m_pendingReadBuffer.end(),
		m_recvBuffer.begin(),
		m_recvBuffer.begin() + bytesReceived
	);

	// Process packets in the buffer
	while( socket->m_pendingReadBuffer.size() >= 4 )
	{
		int32_t packetSize = ntohl( *reinterpret_cast< const int32_t * >( &socket->m_pendingReadBuffer[ 0 ] ) );

		// The length counts its own four bytes, so 1 to 3 is as impossible as 0: the
		// payload size computed below would come out negative.
		if( packetSize < 4 || packetSize > 2048 )
		{
			Log::Error( "Invalid packet size: {}. Disconnecting client.", packetSize );
			socket->flag.disconnected_wait = true;
			socket->m_pendingReadBuffer.clear();
			break;
		}

		if( socket->m_pendingReadBuffer.size() < static_cast< size_t >( packetSize ) )
		{
			break;
		}

		auto stream = std::make_shared< ByteBuffer >( socket->m_pendingReadBuffer.data() + 4, packetSize - 4 );

		// Erase the packet from the buffer
		socket->m_pendingReadBuffer.erase(
			socket->m_pendingReadBuffer.begin(),
			socket->m_pendingReadBuffer.begin() + packetSize
		);

		// A frame with no payload is the game's keep-alive: it sends one when it has
		// sent nothing else for more than ten seconds. There is nothing to answer, but
		// a request may follow it in the same read.
		if( stream->m_buffer.size() == 0 ) continue;

		// Process the packet
		HandleRequest( socket, stream );
	}
}

void LobbyServer::WriteSocket( sptr_socket socket )
{
	if( socket->flag.disconnected_forced || socket->m_pendingWriteBuffer.empty() )
		return;

	socket->last_send_time = std::chrono::steady_clock::now();

	size_t totalBytesSent = 0;
	const size_t bufferSize = socket->m_pendingWriteBuffer.size();

	while( totalBytesSent < bufferSize )
	{
		const size_t remaining = bufferSize - totalBytesSent;
		const int chunkSize = static_cast< int >( std::min<size_t>( remaining, 1024 ) );

		int bytesSent = send(
			socket->fd,
			reinterpret_cast< const char * >( socket->m_pendingWriteBuffer.data() + totalBytesSent ),
			chunkSize,
			0
		);

		if( bytesSent == SOCKET_ERROR )
		{
			const int err = GetSocketError();
			if( SocketWouldBlock( err ) )
				break;

			// Nothing more can be delivered, so there is nothing to wait for. (Waiting
			// would also repeat this error on every pass until the client timed out.)
			Log::Error( "Send failed: {}", err );
			socket->flag.disconnected_forced = true;
			return;
		}

		if( bytesSent == 0 )
		{
			socket->flag.disconnected_forced = true;
			return;
		}

		totalBytesSent += bytesSent;

		if( bytesSent < chunkSize )
		{
			break;
		}
	}

	std::lock_guard< std::mutex > lock( socket->write_mutex );

	socket->m_pendingWriteBuffer.erase(
		socket->m_pendingWriteBuffer.begin(),
		socket->m_pendingWriteBuffer.begin() + totalBytesSent
	);
}

void LobbyServer::HandleRequest( sptr_socket socket, sptr_byte_stream stream )
{
	// A message that is cut short, or whose length fields point past its end, makes
	// the readers throw. Such a message is dropped; the connection and the server go on.
	try
	{
		auto packetId = stream->read< uint16_t >();
		stream->set_position( 0 );

		auto it = REQUEST_EVENT.find( packetId );
		if( it == REQUEST_EVENT.end() )
		{
			Log::Error( "[LOBBY] Unknown packet id : {}", packetId );
			Log::Packet( stream->m_buffer, stream->m_buffer.size(), false );
			return;
		}

		auto request = it->second();

		if( auto res = request->ProcessRequest( socket, stream ) )
		{
			socket->send( res );
		}
	}
	catch( const std::exception &e )
	{
		Log::Error( "[LOBBY] Malformed message from {} dropped : {}", socket->remote_ip, std::string( e.what() ) );
		Log::Packet( stream->m_buffer, stream->m_buffer.size(), false );
	}
	catch( ... )
	{
		Log::Error( "[LOBBY] Malformed message from {} dropped", socket->remote_ip );
	}
}