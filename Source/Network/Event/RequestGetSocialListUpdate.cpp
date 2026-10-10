#include "Network/Event/RequestGetSocialListUpdate.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/RealmUser.hpp"
#include "logging.hpp"

void RequestGetSocialListUpdate::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );
}

sptr_generic_response RequestGetSocialListUpdate::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindUserBySocket( socket );
	if( user == nullptr )
	{
		Log::Error( "RequestGetFriendList::ProcessRequest() - User not found" );
		return std::make_shared< ResultGetSocialListUpdate >( this );
	}

	return std::make_shared< ResultGetSocialListUpdate >( this, user );
}

ResultGetSocialListUpdate::ResultGetSocialListUpdate( GenericRequest *request, sptr_user user ) : GenericResponse( *request )
{
	m_user = user;
}

void ResultGetSocialListUpdate::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( 0 );

	// Sent to a gateway port there is no user, and so two empty lists.
	if( m_user == nullptr )
	{
		out.write_u32( 0 );
		out.write_u32( 0 );
		return;
	}

	// Friends
	out.write_u32( static_cast< uint32_t >( m_user->m_friendList.size() ) );
	for( const auto &friendHandle : m_user->m_friendList )
	{
		out.write_utf16( friendHandle );
	}

	// Friends who are online. This message is the game's friend list request; the
	// ignore list has one of its own, 0x63 (RequestGetIgnoreList).
	const auto onlineFriends = UserManager::Get().GetOnlineFriends( m_user );

	out.write_u32( static_cast< uint32_t >( onlineFriends.size() ) );
	for( const auto &friendHandle : onlineFriends )
	{
		out.write_utf16( friendHandle );
	}
}
