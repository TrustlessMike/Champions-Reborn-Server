#include "Network/Event/RequestMatchGame_RTA.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/GameSessionManager.hpp"
#include "configuration.hpp"
#include "logging.hpp"

void RequestMatchGame_RTA::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );
}

sptr_generic_response RequestMatchGame_RTA::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	// The game repeats this request for as long as its game list is on the screen,
	// also after it has lost its connection and connected again by itself, which it
	// does without logging in. A session that could be picked up again has been by
	// now (LobbyServer::ResumeSession). For any other connection without a login the
	// answer is "invalid session", and the connection stays open: the game takes that
	// result as fatal and tells the player, whereas a list that looks valid followed
	// by a forced logout and a close made it connect and ask again without end.
	auto user = UserManager::Get().FindUserBySocket( socket );
	if( user == nullptr || !user->m_isLoggedIn )
	{
		return std::make_shared< ResultMatchGame_RTA >( this, INVALID_SESSION, "" );
	}

	return std::make_shared< ResultMatchGame_RTA >( this, SUCCESS, socket->remote_ip );
}

ResultMatchGame_RTA::ResultMatchGame_RTA( GenericRequest *request, int32_t reply, std::string userIp ) : GenericResponse( *request )
{
	m_reply = reply;
	m_userIp = userIp;
}

void ResultMatchGame_RTA::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );

	if( m_reply != 0 )
	{
		// The game reads the eight arrays whatever the result is, and reports a
		// result of its own instead of this one when they are missing.
		for( int i = 0; i < 8; i++ )
			out.write_u32( 0 );

		return;
	}

	const auto publicGameList = GameSessionManager::Get().GetAvailableGameSessionList( RealmGameType::RETURN_TO_ARMS );
	const auto publicGameCount = static_cast< uint32_t >( publicGameList.size() );

	out.write_u32( publicGameCount );
	{
		for( const auto &game : publicGameList )
			out.write_utf16( Util::UTF8ToWide( game->m_gameData ) );
	}

	out.write_u32( publicGameCount );
	{
		for( const auto &game : publicGameList )
			out.write_utf16( game->m_playerCount );
	}

	out.write_u32( publicGameCount );
	{
		for( const auto &game : publicGameList )
			out.write_u32( game->m_gameId );
	}

	// One blob per game, like every other array here. The game reads a length and
	// that many bytes for each entry, so an empty blob is a single zero length.
	out.write_u32( publicGameCount );
	{
		for( const auto &game : publicGameList )
			out.write_u32( 0 );	// Size
	}

	out.write_u32( publicGameCount );
	{
		for( const auto &game : publicGameList )
			out.write_utf16( Util::UTF8ToWide( game->m_hostLocalAddr ) );
	}

	out.write_u32( publicGameCount );
	{
		for( const auto &game : publicGameList )
			out.write_u32( game->m_hostLocalPort );
	}

	out.write_u32( publicGameCount );
	{
		for( const auto &game : publicGameList )
			out.write_utf16( Util::UTF8ToWide( Config::ReportedAddress( game->m_hostExternalAddr, m_userIp ) ) );
	}

	out.write_u32( publicGameCount );
	{
		for( const auto &game : publicGameList )
			out.write_u32( game->m_hostNatPort );
	}
}
