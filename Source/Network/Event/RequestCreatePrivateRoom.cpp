#include "Network/Event/RequestCreatePrivateRoom.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/ChatRoomManager.hpp"
#include "logging.hpp"

void RequestCreatePrivateRoom::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	m_sessionId = stream->read_encrypted_utf16();
	m_roomName = stream->read_utf16();
}

sptr_generic_response RequestCreatePrivateRoom::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindUserBySocket( socket );
	if( user == nullptr )
	{
		return std::make_shared< ResultCreatePrivateRoom >( this, GENERAL_ERROR, nullptr );
	}

	auto result = ChatRoomManager::Get().CreateGameChatSession( user, m_roomName );

	if( !result )
	{
		Log::Error( "RequestCreatePrivateRoom::ProcessRequest() - Failed to create private room!" );
		return std::make_shared< ResultCreatePrivateRoom >( this, GENERAL_ERROR, nullptr );
	}

	return std::make_shared< ResultCreatePrivateRoom >( this, SUCCESS, ChatRoomManager::Get().FindRoom( m_roomName ) );
}

ResultCreatePrivateRoom::ResultCreatePrivateRoom( GenericRequest *request, int32_t reply, sptr_chat_room_session room ) : GenericResponse( *request )
{
	m_reply = reply;
	m_room = room;
}

void ResultCreatePrivateRoom::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );

	// The game reads the same four fields here as in the reply to entering a room
	// (0x0C), whatever the result: name, description, members, moderators. On
	// success it goes on to send 0x19 with a description for the room.
	if( m_room == nullptr )
	{
		out.write_utf16( L"" );
		out.write_utf16( L"" );
		out.write_u32( 0 );
		out.write_u32( 0 );
		return;
	}

	out.write_utf16( m_room->m_name );
	out.write_utf16( m_room->m_banner );

	out.write_u32( static_cast< uint32_t >( m_room->m_members.size() ) );
	for( const auto &m : m_room->m_members )
	{
		if( auto member = m.lock() )
		{
			out.write_utf16( member->m_chatHandle );
		}
		else
		{
			out.write_utf16( L"" );
		}
	}

	out.write_u32( static_cast< uint32_t >( m_room->m_moderators.size() ) );
	for( const auto &m : m_room->m_moderators )
	{
		if( auto member = m.lock() )
		{
			out.write_utf16( member->m_chatHandle );
		}
		else
		{
			out.write_utf16( L"" );
		}
	}
}
