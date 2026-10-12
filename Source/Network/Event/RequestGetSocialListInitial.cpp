#include "Network/Event/RequestGetSocialListInitial.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/RealmUser.hpp"
#include "logging.hpp"

void RequestGetSocialListInitial::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );
}

sptr_generic_response RequestGetSocialListInitial::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindLoggedInUser( socket );
	if( user == nullptr )
	{
		Log::Error( "RequestGetFriendList::ProcessRequest() - User not found" );
		return std::make_shared< ResultGetSocialListInitial >( this );
	}

	return std::make_shared< ResultGetSocialListInitial >( this, user );
}

ResultGetSocialListInitial::ResultGetSocialListInitial( GenericRequest *request, sptr_user user ) : GenericResponse( *request )
{
	m_user = user;
}

void ResultGetSocialListInitial::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( 0 );

	// Sent to a gateway port there is no user, and so three empty lists.
	if( m_user == nullptr )
	{
		out.write_u32( 0 );
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

	// Friends who are online. The game marks each name in this list as online in
	// the list above; it hands both to the same function as the reply to 0x62.
	const auto onlineFriends = UserManager::Get().GetOnlineFriends( m_user );

	out.write_u32( static_cast< uint32_t >( onlineFriends.size() ) );
	for( const auto &friendHandle : onlineFriends )
	{
		out.write_utf16( friendHandle );
	}

	// Ignore
	out.write_u32( static_cast< uint32_t >( m_user->m_ignoreList.size() ) );
	for( const auto &ignoreHandle : m_user->m_ignoreList )
	{
		out.write_utf16( ignoreHandle );
	}
}
