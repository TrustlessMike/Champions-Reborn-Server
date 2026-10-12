// ╔═╗ ╦ ╦ ╔═╗ ╔╦╗ ╔═╗ ╦ ╔═╗ ╔╗╔ ╔═╗
// ║   ╠═╣ ╠═╣ ║║║ ╠═╝ ║ ║ ║ ║║║ ╚═╗
// ╚═╝ ╩ ╩ ╩ ╩ ╩ ╩ ╩   ╩ ╚═╝ ╝╚╝ ╚═╝
// ╦  ╔═╗╔╗ ╔╗ ╦ ╦  ╔═╗╔═╗╦═╗╦  ╦╔═╗╦═╗
// ║  ║ ║╠╩╗╠╩╗╚╦╝  ╚═╗║╣ ╠╦╝╚╗╔╝║╣ ╠╦╝
// ╩═╝╚═╝╚═╝╚═╝ ╩   ╚═╝╚═╝╩╚═ ╚╝ ╚═╝╩╚═

#include "Lobby Server/LobbyServer.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <unordered_set>
#include <vector>

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
		auto gatewaySocket = OpenListenerSocket( ip, Config::con_gateway_port + i, RealmGameType::CHAMPIONS_OF_NORRATH );
		if( nullptr == gatewaySocket ) return;

		gatewaySocket->flag.is_gateway = true;
		m_gatewaySockets.push_back( gatewaySocket );
	}

	// Return to Arms Gateway Sockets
	for( int i = 0; i < 8; i++ )
	{
		auto gatewaySocket = OpenListenerSocket( ip, Config::rta_gateway_port + i, RealmGameType::RETURN_TO_ARMS );
		if( nullptr == gatewaySocket ) return;

		gatewaySocket->flag.is_gateway = true;
		m_gatewaySockets.push_back( gatewaySocket );
	}

	m_conSocket = OpenListenerSocket( ip, Config::con_lobby_port, RealmGameType::CHAMPIONS_OF_NORRATH );
	m_rtaSocket = OpenListenerSocket( ip, Config::rta_lobby_port, RealmGameType::RETURN_TO_ARMS );

	// Run() uses both without asking; main() ends the program when the lobby is not running.
	if( nullptr == m_conSocket || nullptr == m_rtaSocket ) return;

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

	m_lastSocketCheck = std::chrono::steady_clock::now();

	size_t pass = 0;

	while( m_running )
	{
		// select() may modify the timeout, and outside Windows it needs the highest fd.
		timeval timeout = { 0, 1000 };
		SOCKET maxFd = 0;

		FD_ZERO( &readSet );
		FD_ZERO( &writeSet );

		// std::max is called with its template argument throughout this function:
		// <windows.h> defines max( a, b ) as a macro, which a plain call would expand.
		FD_SET( m_conSocket->fd, &readSet );
		FD_SET( m_rtaSocket->fd, &readSet );
		maxFd = std::max< SOCKET >( { maxFd, m_conSocket->fd, m_rtaSocket->fd } );

		for( const auto &sock : m_gatewaySockets )
		{
			if( sock != nullptr && sock->fd != INVALID_SOCKET )
			{
				FD_SET( sock->fd, &readSet );
				maxFd = std::max< SOCKET >( maxFd, sock->fd );
			}
		}

		const auto beforeSelect = std::chrono::steady_clock::now();

		for( const auto &client : m_clientSockets )
		{
			// A connection whose address has used up its budget is left unread until
			// there is budget again: what it sends meanwhile waits in the socket.
			if( beforeSelect >= client->paused_until )
			{
				FD_SET( client->fd, &readSet );
			}

			maxFd = std::max< SOCKET >( maxFd, client->fd );

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

			// By index: the list does not change while requests are handled (a
			// connection that has to go is only marked, and removed further down),
			// but nothing here should depend on that. Each pass starts one connection
			// further on, so that when an address is short of budget its connections
			// take turns instead of the oldest being served first every time.
			const size_t clientCount = m_clientSockets.size();
			const size_t firstClient = clientCount > 0 ? ( pass++ % clientCount ) : 0;

			for( size_t k = 0; k < clientCount; ++k )
			{
				const size_t i = ( firstClient + k ) % clientCount;

				if( i >= m_clientSockets.size() )
				{
					continue;
				}

				const sptr_socket client = m_clientSockets[ i ];

				if( client->fd == INVALID_SOCKET )
				{
					continue;
				}

				if( FD_ISSET( client->fd, &readSet ) )
				{
					ReadSocket( client );
				}
				else if( client->m_pendingReadBuffer.size() >= 4 )
				{
					// Requests that had to wait for budget.
					ProcessFrames( client );
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

		// Only now, with everything the clients had sent taken in, is a silent
		// connection timed out. Checking before the read dropped clients whose data
		// was waiting in the socket when the server had not run for a while.
		try
		{
			CheckSocketProblem();
		}
		catch( const std::exception &e )
		{
			Log::Error( "[LOBBY] Unhandled exception while closing connections: {}", std::string( e.what() ) );
		}
		catch( ... )
		{
			Log::Error( "[LOBBY] Unhandled exception of unknown type while closing connections" );
		}
	}
}

void LobbyServer::CheckSocketProblem()
{
	auto now = std::chrono::steady_clock::now();

	// The idle timeout is for a client that says nothing while the server listens.
	// When the host computer has been asleep, or the process stopped, the clock has
	// moved on by the whole pause although nobody could have been heard. Such a pause
	// shows as a long gap between two checks, and is given back to every client.
	const auto sinceLastCheck = now - m_lastSocketCheck;
	const bool serverPaused = sinceLastCheck > std::chrono::seconds( MAX_SERVER_PAUSE_SECONDS );
	m_lastSocketCheck = now;

	if( serverPaused )
	{
		Log::Info( "[LOBBY] Server stood still for {} seconds, not counted as client silence",
			std::chrono::duration_cast< std::chrono::seconds >( sinceLastCheck ).count() );
	}

	for( size_t i = 0; i < m_clientSockets.size(); )
	{
		const sptr_socket socket = m_clientSockets[ i ];

		if( serverPaused )
		{
			socket->last_recv_time += sinceLastCheck;
			socket->connected_time += sinceLastCheck;
			socket->close_wait_since += sinceLastCheck;

			// Not beyond the present: a client heard since the pause is simply up to date.
			if( socket->last_recv_time > now )
			{
				socket->last_recv_time = now;
			}

			if( socket->connected_time > now )
			{
				socket->connected_time = now;
			}

			if( socket->close_wait_since > now )
			{
				socket->close_wait_since = now;
			}
		}

		auto elapsed = std::chrono::duration_cast< std::chrono::seconds >( now - socket->last_recv_time );

		// Check for client timeouts
		if( !socket->flag.disconnected_forced && elapsed.count() > Config::client_timeout )
		{
			socket->flag.disconnected_forced = true;
			Log::Info( "[LOBBY] Client Timeout : ({})", socket->remote_ip );
		}

		// A console says what it wants as soon as it is connected. A connection that
		// has not sent one whole message after this long is not a console.
		if( !socket->flag.disconnected_forced && socket->frames_received == 0
			&& now - socket->connected_time > std::chrono::seconds( Config::first_message_timeout ) )
		{
			socket->flag.disconnected_forced = true;
			Log::Info( "[LOBBY] Client sent nothing after connecting : ({})", socket->remote_ip );
		}

		// Check if we're waiting to disconnect after sending buffered data. The wait
		// has an end: a client that does not take its last replies is closed anyway.
		if( socket->flag.disconnected_wait )
		{
			if( !socket->close_wait_started )
			{
				socket->close_wait_started = true;
				socket->close_wait_since = now;
			}

			if( socket->m_pendingWriteBuffer.empty()
				|| now - socket->close_wait_since > std::chrono::seconds( MAX_CLOSE_WAIT_SECONDS ) )
			{
				socket->flag.disconnected_forced = true;
			}
		}

		// Replies pile up here when the client does not read them.
		if( !socket->flag.disconnected_forced && socket->m_pendingWriteBuffer.size() > MAX_PENDING_WRITE_BYTES )
		{
			socket->flag.disconnected_forced = true;
			Log::Info( "[LOBBY] Client is not reading its replies : ({})", socket->remote_ip );
		}

		if( socket->flag.disconnected_forced )
		{
			m_clientSockets.erase( m_clientSockets.begin() + i );
			DropSocket( socket );
		}
		else
		{
			++i;
		}
	}
}

// The connection is no longer in m_clientSockets; this ends what hangs on it.
void LobbyServer::DropSocket( sptr_socket socket )
{
	// Close first, and whatever happens below: the connection must not outlive the
	// list, and something else may still hold a reference to the socket.
	if( socket->fd != INVALID_SOCKET )
	{
		closesocket( socket->fd );
		socket->fd = INVALID_SOCKET;
	}

	socket->flag.disconnected_forced = true;

	// Nothing more will be sent or read; a reference that outlives the connection
	// (an unfinished character upload names its users) must not hold the buffers.
	{
		std::lock_guard< std::mutex > lock( socket->write_mutex );
		std::vector< uint8_t >().swap( socket->m_pendingWriteBuffer );
	}

	std::vector< uint8_t >().swap( socket->m_pendingReadBuffer );

	if( !socket->limits_exempt )
	{
		auto it = m_addresses.find( socket->address_key );
		if( it != m_addresses.end() && it->second.m_connections > 0 )
		{
			it->second.m_connections--;
		}
	}

	// Not a logout: a user that was logged in can pick its session up again.
	UserManager::Get().OnConnectionLost( socket );
	Log::Info( "[LOBBY] Client Disconnected : ({})", socket->remote_ip );
}

// Called with the list full. Gives up the connection that has not logged in and has
// been silent the longest, if there is one: somebody who only holds connections open
// then loses them to whoever comes next, and players who are logged in are not touched.
bool LobbyServer::MakeRoomForConnection()
{
	// The connections that are logged in, looked up once.
	std::unordered_set< const RealmSocket * > loggedIn;

	for( const auto &user : UserManager::Get().GetUserList() )
	{
		if( user->m_isLoggedIn && user->sock != nullptr )
		{
			loggedIn.insert( user->sock.get() );
		}
	}

	size_t victim = m_clientSockets.size();

	for( size_t i = 0; i < m_clientSockets.size(); ++i )
	{
		const auto &candidate = m_clientSockets[ i ];

		if( loggedIn.count( candidate.get() ) != 0 )
		{
			continue;
		}

		if( victim == m_clientSockets.size() || candidate->last_recv_time < m_clientSockets[ victim ]->last_recv_time )
		{
			victim = i;
		}
	}

	if( victim == m_clientSockets.size() )
	{
		return false;
	}

	const sptr_socket socket = m_clientSockets[ victim ];
	Log::Info( "[LOBBY] Too many connections, closing the one that has waited longest without logging in : ({})", socket->remote_ip );

	m_clientSockets.erase( m_clientSockets.begin() + victim );
	DropSocket( socket );

	return true;
}

// Addresses nobody is connected from are forgotten once their allowances have had
// time to fill up again, so that the table cannot grow for ever.
void LobbyServer::ForgetIdleAddresses()
{
	const auto now = std::chrono::steady_clock::now();
	const auto keep = std::chrono::seconds( static_cast< int64_t >( ACCOUNT_CREATION_BURST * ACCOUNT_CREATION_SECONDS ) );

	for( auto it = m_addresses.begin(); it != m_addresses.end(); )
	{
		if( it->second.m_connections <= 0 && now - it->second.m_lastSeen > keep )
		{
			it = m_addresses.erase( it );
		}
		else
		{
			++it;
		}
	}
}

bool LobbyServer::AllowAccountCreation( const sptr_socket &socket )
{
	if( socket == nullptr || socket->limits_exempt )
	{
		return true;
	}

	auto &address = m_addresses[ socket->address_key ];
	const auto now = std::chrono::steady_clock::now();

	address.m_lastSeen = now;
	address.m_accountCreations.Refill( now, 1.0 / ACCOUNT_CREATION_SECONDS, ACCOUNT_CREATION_BURST );

	if( !address.m_accountCreations.Has( 1.0 ) )
	{
		return false;
	}

	address.m_accountCreations.Take( 1.0 );
	return true;
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

	const std::string remoteIp = Util::IPFromAddr( clientInfo );
	const uint32_t addressKey = ntohl( clientInfo.sin_addr.s_addr );
	const bool exempt = !Config::limits_for_local_addresses && Util::IsPrivateOrLoopbackIPv4( remoteIp );

	// One address may hold only so many connections.
	if( !exempt )
	{
		if( m_addresses.size() > 4096 )
		{
			ForgetIdleAddresses();
		}

		auto &address = m_addresses[ addressKey ];
		address.m_lastSeen = std::chrono::steady_clock::now();

		if( address.m_connections >= Config::max_connections_per_address )
		{
			Log::Info( "[LOBBY] Too many connections from ({}), refusing a new one", remoteIp );
			closesocket( clientSocket );
			return;
		}
	}

	// And the server only so many in all.
	if( m_clientSockets.size() >= static_cast< size_t >( Config::max_connections ) && !MakeRoomForConnection() )
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
	new_socket->remote_ip = remoteIp;
	new_socket->remote_port = ntohs( clientInfo.sin_port );
	new_socket->gameType = gameType;
	new_socket->address_key = addressKey;
	new_socket->limits_exempt = exempt;
	m_clientSockets.push_back( new_socket );

	if( !exempt )
	{
		m_addresses[ addressKey ].m_connections++;
	}

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

	// A connection that is being closed is read only so that its end is noticed.
	if( socket->flag.disconnected_wait )
	{
		return;
	}

	// Append received data to socket's pending buffer
	socket->m_pendingReadBuffer.insert(
		socket->m_pendingReadBuffer.end(),
		m_recvBuffer.begin(),
		m_recvBuffer.begin() + bytesReceived
	);

	ProcessFrames( socket );
}

// What a request costs its sender's address (configuration.hpp, requests_per_second).
double LobbyServer::RequestCost( uint16_t packetId )
{
	switch( packetId )
	{
		case 0x0006:	// Create account: a password hash and a database write
			return 20.0;

		case 0x0016:	// Login: a password hash
		case 0x0064:	// Account details: the same
		case 0x005C:	// Character data: a database read and 20 KB of replies
			return 10.0;

		case 0x0001:	// Friends and ignore lists: a database write each
		case 0x0002:
		case 0x001C:
		case 0x001D:
		case 0x005A:	// Delete a character
		case 0x0008:	// Games and chat rooms opened
		case 0x0009:
		case 0x000A:
		case 0x0054:
		case 0x0056:
			return 5.0;

		case 0x0058:	// A character upload begins (the write is charged when it happens)
		case 0x005E:
		case 0x005B:	// Character list: a database read
			return 3.0;

		case 0x0021:	// Chat: one message out for every listener
		case 0x0022:
			return 2.0;
	}

	return 1.0;
}

void LobbyServer::ProcessFrames( sptr_socket socket )
{
	// Process packets in the buffer
	while( socket->m_pendingReadBuffer.size() >= 4 )
	{
		if( socket->flag.disconnected_forced || socket->flag.disconnected_wait )
		{
			socket->m_pendingReadBuffer.clear();
			break;
		}

		// Read byte by byte: the length is big-endian whatever the host is.
		const uint8_t *head = socket->m_pendingReadBuffer.data();
		const uint32_t rawSize = ( static_cast< uint32_t >( head[ 0 ] ) << 24 ) | ( static_cast< uint32_t >( head[ 1 ] ) << 16 )
			| ( static_cast< uint32_t >( head[ 2 ] ) << 8 ) | static_cast< uint32_t >( head[ 3 ] );

		// The length counts its own four bytes, so 1 to 3 is as impossible as 0: the
		// payload size computed below would come out negative.
		if( rawSize < 4 || rawSize > 2048 )
		{
			Log::Error( "Invalid packet size: {}. Disconnecting client.", static_cast< int32_t >( rawSize ) );
			socket->flag.disconnected_wait = true;
			socket->m_pendingReadBuffer.clear();
			break;
		}

		const size_t packetSize = rawSize;

		if( socket->m_pendingReadBuffer.size() < packetSize )
		{
			break;
		}

		const auto now = std::chrono::steady_clock::now();
		const bool keepAlive = ( packetSize == 4 );
		AddressState *address = nullptr;

		if( !socket->limits_exempt )
		{
			if( now < socket->paused_until )
			{
				break;
			}

			address = &m_addresses[ socket->address_key ];
			address->m_lastSeen = now;
			address->m_requests.Refill( now, Config::requests_per_second, Config::request_burst );

			// Paid for afterwards, so all a request needs to be let in is a budget
			// that is not overdrawn.
			if( !address->m_requests.Has( 1.0 ) )
			{
				socket->paused_until = now + address->m_requests.WaitFor( 1.0, Config::requests_per_second );
				break;
			}

			const uint16_t packetId = packetSize >= 6 ? static_cast< uint16_t >( head[ 4 ] | ( head[ 5 ] << 8 ) ) : 0;

			// Password checks have an allowance of their own: guessing has to be slow
			// whatever else the address may still do.
			if( packetId == 0x0016 || packetId == 0x0064 )
			{
				address->m_passwordChecks.Refill( now, 1.0 / PASSWORD_CHECK_SECONDS, PASSWORD_CHECK_BURST );

				if( !address->m_passwordChecks.Has( 1.0 ) )
				{
					socket->paused_until = now + address->m_passwordChecks.WaitFor( 1.0, 1.0 / PASSWORD_CHECK_SECONDS );
					break;
				}

				address->m_passwordChecks.Take( 1.0 );
			}

			address->m_requests.Take( keepAlive ? 0.1 : RequestCost( packetId ) );
		}

		// Only a whole frame counts as a sign of life.
		socket->last_recv_time = now;
		socket->frames_received++;

		auto stream = std::make_shared< ByteBuffer >( socket->m_pendingReadBuffer.data() + 4, static_cast< uint32_t >( packetSize - 4 ) );

		// Erase the packet from the buffer
		socket->m_pendingReadBuffer.erase(
			socket->m_pendingReadBuffer.begin(),
			socket->m_pendingReadBuffer.begin() + packetSize
		);

		// A frame with no payload is the game's keep-alive: it sends one when it has
		// sent nothing else for more than ten seconds. There is nothing to answer, but
		// a request may follow it in the same read.
		if( keepAlive ) continue;

		// Process the packet
		m_extraCost = 0;
		HandleRequest( socket, stream );

		if( m_extraCost > 0 && !socket->limits_exempt )
		{
			// The table may have been rehashed by now; look the address up again.
			m_addresses[ socket->address_key ].m_requests.Take( m_extraCost );
		}

		m_extraCost = 0;
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

		ResumeSession( socket, stream, packetId );

		if( auto res = request->ProcessRequest( socket, stream ) )
		{
			socket->send( res );
		}
	}
	catch( const std::exception &e )
	{
		Log::Error( "[LOBBY] Malformed message from {} dropped : {}", socket->remote_ip, std::string( e.what() ) );
		Log::Packet( stream->m_buffer, stream->m_buffer.size(), false );
		m_extraCost += 1;
	}
	catch( ... )
	{
		Log::Error( "[LOBBY] Malformed message from {} dropped", socket->remote_ip );
		m_extraCost += 1;
	}
}

void LobbyServer::ResumeSession( sptr_socket socket, sptr_byte_stream stream, uint16_t packetId )
{
	// A game that has lost its connection connects again by itself and carries on
	// under the session id it had, without logging in again. Every request it sends
	// once it is logged in starts with that id; the ones of the connect and login
	// sequence do not.
	switch( packetId )
	{
		case 0x0006:	// Create account
		case 0x000F:	// Realm statistics
		case 0x0016:	// Login
		case 0x0027:	// Encryption key
		case 0x0042:	// Rules
		case 0x0043:	// Server address
		case 0x0064:	// Account details
			return;
	}

	auto user = UserManager::Get().FindUserBySocket( socket );
	if( user == nullptr || user->m_isLoggedIn )
	{
		return;
	}

	std::wstring sessionId;

	try
	{
		stream->read_u16();		// Message id
		stream->read_u32();		// Tracking number
		stream->read_u32();		// Version

		sessionId = stream->read_encrypted_utf16();
	}
	catch( const std::exception & )
	{
		// Too short to hold a session id. The request's own handler deals with that.
	}

	// The request is handled next, and must find the message as it arrived.
	stream->set_position( 0 );

	UserManager::Get().ResumeSession( user, sessionId );
}