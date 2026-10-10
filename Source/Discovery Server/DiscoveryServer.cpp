// ╔═╗ ╦ ╦ ╔═╗ ╔╦╗ ╔═╗ ╦ ╔═╗ ╔╗╔ ╔═╗
// ║   ╠═╣ ╠═╣ ║║║ ╠═╝ ║ ║ ║ ║║║ ╚═╗
// ╚═╝ ╩ ╩ ╩ ╩ ╩ ╩ ╩   ╩ ╚═╝ ╝╚╝ ╚═╝                      
// ╔╦╗╦╔═╗╔═╗╔═╗╦  ╦╔═╗╦═╗╦ ╦  ╔═╗╔═╗╦═╗╦  ╦╔═╗╦═╗
//  ║║║╚═╗║  ║ ║╚╗╔╝║╣ ╠╦╝╚╦╝  ╚═╗║╣ ╠╦╝╚╗╔╝║╣ ╠╦╝
// ═╩╝╩╚═╝╚═╝╚═╝ ╚╝ ╚═╝╩╚═ ╩   ╚═╝╚═╝╩╚═ ╚╝ ╚═╝╩╚═

#include "Discovery Server/DiscoveryServer.hpp"

#include <algorithm>
#include <chrono>

#include "Common/ByteStream.hpp"
#include "Game/RealmUserManager.hpp"
#include "Game/GameSessionManager.hpp"
#include "Lobby Server/LobbyServer.hpp"
#include "logging.hpp"

DiscoveryServer::DiscoveryServer()
{
	m_running = false;

	m_socket = INVALID_SOCKET;
	m_recvBuffer.resize( 1024 );
}

DiscoveryServer::~DiscoveryServer()
{
	Log::Info( "Discovery Server stopped." );
}

void DiscoveryServer::Start( std::string ip, int32_t port )
{
	m_socket = socket( AF_INET, SOCK_DGRAM, 0 );

	if( m_socket == INVALID_SOCKET )
	{
		Log::Error( "Failed to create socket." );
		return;
	}

	sockaddr_in service{};
	service.sin_family = AF_INET;
	service.sin_port = htons( port );

	if( ip == "0.0.0.0" )
	{
		service.sin_addr.s_addr = INADDR_ANY;
	}
	else
	{
		if( InetPtonA( AF_INET, ip.c_str(), &service.sin_addr ) != 1 )
		{
			Log::Error( "Invalid IP address format: {}", ip );
			closesocket( m_socket );
			return;
		}
	}

	if( bind( m_socket, reinterpret_cast< SOCKADDR * >( &service ), sizeof( service ) ) == SOCKET_ERROR )
	{
		Log::Error( "Failed to bind socket." );
		closesocket( m_socket );
		return;
	}

#ifndef _WIN32
	// Wake up periodically so Stop() can join the thread without waiting for a packet.
	timeval recvTimeout = { 0, 250000 };
	setsockopt( m_socket, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof( recvTimeout ) );
#endif

	m_running = true;
	m_thread = std::thread( &DiscoveryServer::Run, this );

	Log::Info( "Discovery Server started {}:{}", ip, port );
}

void DiscoveryServer::Stop()
{
	m_running = false;
	if( m_thread.joinable() )
	{
		m_thread.join();
	}
}

void DiscoveryServer::Run()
{
	sockaddr_in clientAddr;

	while( m_running )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

		// recvfrom() overwrites the length, so it has to be reset for every packet.
		socklen_t clientAddrLen = sizeof( clientAddr );

		auto bytesReceived = recvfrom( m_socket, ( char * )m_recvBuffer.data(), 1024, 0, ( struct sockaddr * )&clientAddr, &clientAddrLen );

		if( bytesReceived == SOCKET_ERROR || bytesReceived < 4 )
		{
			continue;
		}

		// The lobby thread works on the same users and games.
		std::lock_guard< std::mutex > stateLock( LobbyServer::StateMutex() );

		// A datagram can come from anyone and contain anything; none may stop this thread.
		try
		{
			ProcessPacket( &clientAddr, std::make_shared< ByteBuffer >( m_recvBuffer.data(), bytesReceived ) );
		}
		catch( const std::exception &e )
		{
			Log::Error( "[DISCOVERY] Malformed datagram dropped : {}", std::string( e.what() ) );
		}
		catch( ... )
		{
			Log::Error( "[DISCOVERY] Malformed datagram dropped" );
		}
	}
}

void DiscoveryServer::ProcessPacket( sockaddr_in *clientAddr, sptr_byte_stream stream )
{
	// 4-byte length plus the 32-byte encrypted session id; anything shorter is junk.
	if( stream->get_length() < 0x24 )
		return;

	if( 0x20 != stream->read_u32() )
		return;

	auto encryptedBytes = stream->read_bytes( 0x20 );
	auto decryptedBytes = RealmCrypt::decryptSymmetric( encryptedBytes );

	// The session id is 16 UTF-16LE code units.
	std::wstring sessionId( 16, L'\0' );
	for( size_t i = 0; i < 16; ++i )
	{
		sessionId[ i ] = static_cast< wchar_t >( decryptedBytes[ i * 2 ] | ( decryptedBytes[ i * 2 + 1 ] << 8 ) );
	}

	// Validate the session ID is 16 characters long and not all 0's
	if( sessionId.size() != 16 || std::all_of( sessionId.begin(), sessionId.end(), []( wchar_t ch )
	{
		return ch == L'\0';
	} ) )
	{
		Log::Error( "Invalid session id." );
		return;
	}

	// Get the users remote IP and Port for discovery.
	char remoteIp[ INET_ADDRSTRLEN ];
	InetNtopA( AF_INET, &clientAddr->sin_addr, remoteIp, INET_ADDRSTRLEN );

	uint16_t remotePort = ntohs( clientAddr->sin_port );

	// Find the user associated with the session ID
	auto user = UserManager::Get().FindUserBySessionId( sessionId );
	if( user == nullptr || user->sock == nullptr )
	{
		Log::Error( "User not found! [{}]", sessionId );
		return;
	}

	if( remoteIp != user->sock->remote_ip )
	{
		Log::Error( "Discovery Handshake from invalid IP!" );
		return;
	}

	// Initialize our discovery information
	user->m_discoveryAddr = remoteIp;
	user->m_discoveryPort = remotePort;

	Log::Debug( "Discovery Handshake from {}:{}", remoteIp, remotePort );

	if( user->m_isHost )
	{
		GameSessionManager::Get().RequestOpen( user );
	}
	else
	{
		GameSessionManager::Get().RequestJoin( user );
	}
}