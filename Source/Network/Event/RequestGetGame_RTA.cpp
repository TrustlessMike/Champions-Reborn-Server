#include "Network/Event/RequestGetGame_RTA.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/GameSessionManager.hpp"
#include "Game/RealmUser.hpp"
#include "configuration.hpp"
#include "logging.hpp"

void RequestGetGame_RTA::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	m_sessionId = stream->read_encrypted_utf16();
	m_gameName = stream->read_utf16();
}

sptr_generic_response RequestGetGame_RTA::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindUserBySocket( socket );
	if( user == nullptr )
	{
		Log::Error( "User not found! [{}]", m_sessionId );
		return std::make_shared< ResultGetGame_RTA >( this, TIMEOUT );
	}

	auto session = GameSessionManager::Get().FindGame( m_gameName, user->m_gameType );

	if( session == nullptr )
	{
		Log::Error( "Game session not found! [{}]", m_gameName );
		return std::make_shared< ResultGetGame_RTA >( this, NOT_FOUND );
	}

	if( session->m_currentPlayers >= session->m_maximumPlayers )
	{
		Log::Error( "Game session is full! [{}]", m_gameName );
		return std::make_shared< ResultGetGame_RTA >( this, TIMEOUT );
	}

	auto host_user = session->GetOwner();

	if( host_user == nullptr )
	{
		Log::Error( "Game session owner not found! [{}]", m_gameName );
		return std::make_shared< ResultGetGame_RTA >( this, TIMEOUT );
	}

	// Until the host's discovery datagram has arrived there is no address to hand out.
	if( session->m_state != GameSession::GameState::Open )
	{
		Log::Error( "Game session is not open yet! [{}]", m_gameName );
		return std::make_shared< ResultGetGame_RTA >( this, NOT_FOUND );
	}

	user->m_isHost = false;
	user->m_gameId = session->m_gameId;

	// The same four values the public game list hands out for this host.
	return std::make_shared< ResultGetGame_RTA >( this, SUCCESS, session->m_gameId,
		Config::ReportedAddress( session->m_hostExternalAddr, socket->remote_ip ), session->m_hostNatPort,
		session->m_hostLocalAddr, session->m_hostLocalPort );
}

ResultGetGame_RTA::ResultGetGame_RTA( GenericRequest *request, int32_t reply, int32_t gameId, std::string discoveryAddr, int32_t discoveryPort, std::string localAddr, int32_t localPort ) : GenericResponse( *request )
{
	m_reply = reply;
	m_gameId = gameId;
	m_discoveryAddr = Util::UTF8ToWide( discoveryAddr );
	m_discoveryPort = discoveryPort;
	m_localAddr = Util::UTF8ToWide( localAddr );
	m_localPort = localPort;
}

void ResultGetGame_RTA::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );

	out.write_utf16( L"Test" );
	out.write_u32( m_gameId );

	// The game takes the first pair as the host's internal (LAN) address and the
	// second as its external one, the same order as in the game list and in 0x65.
	out.write_utf16( m_localAddr );
	out.write_u32( m_localPort );

	out.write_utf16( m_discoveryAddr );
	out.write_u32( m_discoveryPort );
}
