#include "Network/Event/RequestGetAccountInfo.hpp"

#include "Database/Database.hpp"
#include "Game/RealmUserManager.hpp"
#include "Common/Constant.hpp"
#include "logging.hpp"

void RequestGetAccountInfo::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	m_username = stream->read_encrypted_utf16();
	m_password = stream->read_encrypted_utf16();
}

sptr_generic_response RequestGetAccountInfo::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	if( socket->gameType != RealmGameType::RETURN_TO_ARMS || m_username.empty() || m_password.empty() )
	{
		return std::make_shared< ResultGetAccountInfo >( this, ACCOUNT_INVALID );
	}

	auto &Database = Database::Get();

	// The same check as a login: only the owner of the account is told anything.
	auto [ result, accountId, chatHandle ] = Database.VerifyAccount( m_username, m_password );

	if( accountId < 0 )
	{
		Log::Error( "RequestGetAccountInfo::ProcessRequest() - Invalid account ID: " + std::to_string( accountId ) );
		return std::make_shared< ResultGetAccountInfo >( this, ACCOUNT_INVALID );
	}

	auto [ found, emailAddress, dateOfBirth ] = Database.LoadAccount( accountId );

	if( !found )
	{
		return std::make_shared< ResultGetAccountInfo >( this, ACCOUNT_INVALID );
	}

	return std::make_shared< ResultGetAccountInfo >( this, SUCCESS, emailAddress, dateOfBirth );
}

ResultGetAccountInfo::ResultGetAccountInfo( GenericRequest *request, int32_t reply, std::wstring emailAddress, std::wstring dateOfBirth ) : GenericResponse( *request )
{
	m_reply = reply;

	// The game copies at most 49 characters of the first string and 15 of the sixth.
	// What the other ten are for is not known; it reads them and uses none.
	m_fields[ 0 ] = emailAddress;
	m_fields[ 5 ] = dateOfBirth;
}

void ResultGetAccountInfo::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );

	for( const auto &field : m_fields )
	{
		out.write_encrypted_utf16( field );
	}
}
