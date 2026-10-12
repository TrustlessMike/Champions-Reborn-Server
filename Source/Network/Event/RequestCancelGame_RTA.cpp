#include "Network/Event/RequestCancelGame_RTA.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/GameSessionManager.hpp"
#include "Game/ChatRoomManager.hpp"
#include "logging.hpp"

void RequestCancelGame_RTA::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	m_sessionId = stream->read_encrypted_utf16();
}

sptr_generic_response RequestCancelGame_RTA::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindLoggedInUser( socket );
	if( user == nullptr )
	{
		Log::Error( "User not found! [{}]", m_sessionId );
		return std::make_shared< ResultCancelGame_RTA >( this );
	}

	if( user->m_gameType != RealmGameType::RETURN_TO_ARMS )
	{
		return std::make_shared< ResultCancelGame_RTA >( this );
	}

	if( !GameSessionManager::Get().RequestCancel( user ) )
	{
		// The game may already be gone: started, or cancelled by its host.
		Log::Debug( "No game session to cancel for user [{}]", user->m_sessionId );
		user->m_gameId = -1;
	}

	// The game's chat room is left in any case, or its name stays taken.
	if( user->m_privateRoomId >= 0 && !ChatRoomManager::Get().LeaveRoom( user, user->m_privateRoomId ) )
	{
		Log::Error( "Failed to leave private chat room for user [{}]", user->m_username );
		user->m_privateRoomId = -1;
	}

	return std::make_shared< ResultCancelGame_RTA >( this );
}

ResultCancelGame_RTA::ResultCancelGame_RTA( GenericRequest *request ) : GenericResponse( *request )
{
	
}

void ResultCancelGame_RTA::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( 0 );
}
