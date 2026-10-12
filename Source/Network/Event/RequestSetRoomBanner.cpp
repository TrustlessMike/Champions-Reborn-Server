#include "Network/Event/RequestSetRoomBanner.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/ChatRoomManager.hpp"
#include "logging.hpp"

void RequestSetRoomBanner::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	m_sessionId = stream->read_encrypted_utf16();
	m_roomName = stream->read_utf16();
	m_banner = stream->read_utf16();
}

sptr_generic_response RequestSetRoomBanner::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	const auto user = UserManager::Get().FindLoggedInUser( socket );
	if( !user )
	{
		Log::Error( "User not found for socket!" );
		return std::make_shared< ResultSetRoomBanner >( this, GENERAL_ERROR );
	}

	const auto room = ChatRoomManager::Get().FindRoom( m_roomName );
	if( !room )
	{
		Log::Error( "Chat room not found: {}", m_roomName );
		return std::make_shared< ResultSetRoomBanner >( this, GENERAL_ERROR );
	}

	// Only the user who created the room may label it; the public rooms have no owner.
	if( room->m_owner.lock() != user )
	{
		Log::Error( "User [{}] does not own chat room [{}]", user->m_chatHandle, m_roomName );
		return std::make_shared< ResultSetRoomBanner >( this, GENERAL_ERROR );
	}

	// The game refuses a room description whose text is longer than this.
	if( m_banner.size() > 1000 )
	{
		m_banner.resize( 1000 );
	}

	room->m_banner = m_banner;

	return std::make_shared< ResultSetRoomBanner >( this, SUCCESS );
}

ResultSetRoomBanner::ResultSetRoomBanner( GenericRequest *request, int32_t reply ) : GenericResponse( *request )
{
	m_reply = reply;
}

void ResultSetRoomBanner::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );
}
