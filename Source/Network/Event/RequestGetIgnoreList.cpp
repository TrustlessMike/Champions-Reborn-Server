#include "Network/Event/RequestGetIgnoreList.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/RealmUser.hpp"
#include "logging.hpp"

void RequestGetIgnoreList::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );
}

sptr_generic_response RequestGetIgnoreList::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindLoggedInUser( socket );
	if( user == nullptr )
	{
		Log::Error( "RequestGetIgnoreList::ProcessRequest() - User not found" );
		return std::make_shared< ResultGetIgnoreList >( this );
	}

	return std::make_shared< ResultGetIgnoreList >( this, user );
}

ResultGetIgnoreList::ResultGetIgnoreList( GenericRequest *request, sptr_user user ) : GenericResponse( *request )
{
	m_user = user;
}

void ResultGetIgnoreList::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( 0 );

	if( m_user == nullptr )
	{
		out.write_u32( 0 );
		return;
	}

	// Ignore
	out.write_u32( static_cast< uint32_t >( m_user->m_ignoreList.size() ) );
	for( const auto &ignoreHandle : m_user->m_ignoreList )
	{
		out.write_utf16( ignoreHandle );
	}
}
