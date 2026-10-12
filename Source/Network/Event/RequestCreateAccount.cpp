#include "Network/Event/RequestCreateAccount.hpp"

#include "Game/RealmUserManager.hpp"
#include "Game/RealmUser.hpp"
#include "Common/Constant.hpp"
#include "Crypto/PasswordHash.hpp"
#include "Database/Database.hpp"
#include "Lobby Server/LobbyServer.hpp"
#include "logging.hpp"

void RequestCreateAccount::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	m_username = stream->read_encrypted_utf16();
	m_password = stream->read_encrypted_utf16();
	m_emailAddress = stream->read_encrypted_utf16();
	m_dateOfBirth = stream->read_encrypted_utf16();
	m_chatHandle = stream->read_encrypted_utf16();
}

bool RequestCreateAccount::VerifyUserData()
{
	const auto plain = []( const std::wstring &text, size_t longest )
	{
		if( text.empty() || text.length() > longest )
		{
			return false;
		}

		for( const wchar_t ch : text )
		{
			if( ch < 0x20 || ch == 0x7F )
			{
				return false;
			}
		}

		return true;
	};

	if( !plain( m_username, 32 ) || !plain( m_password, 64 ) || !plain( m_emailAddress, 128 )
		|| !plain( m_dateOfBirth, 32 ) || !plain( m_chatHandle, 15 ) )
	{
		Log::Error( "RequestCreateAccount::VerifyUserData() - A field is empty, too long or holds control characters." );
		return false;
	}

	return true;
}

sptr_generic_response RequestCreateAccount::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindUserBySocket( socket );
	if( nullptr == user || user->m_gameType != RealmGameType::RETURN_TO_ARMS )
	{
		return std::make_shared< ResultCreateAccount >( this, ERROR_FATAL, L"" );
	}

	if( m_username.empty() || m_password.empty() || m_emailAddress.empty() || m_dateOfBirth.empty() || m_chatHandle.empty() )
	{
		Log::Error( "RequestCreateAccount::ProcessRequest() - Missing required fields for account creation." );
		return std::make_shared< ResultCreateAccount >( this, ERROR_FATAL, L"" );
	}

	// The game refuses a whole list of names (room members, friends, ignored users)
	// when one name in it has more than 15 characters, so a longer handle would
	// break those lists for everyone who meets its owner.
	if( m_chatHandle.length() > 15 )
	{
		Log::Error( "RequestCreateAccount::ProcessRequest() - Chat handle is longer than 15 characters." );
		return std::make_shared< ResultCreateAccount >( this, ERROR_FATAL, L"" );
	}

	// Nothing the game's own screens produce is refused here: these are the sizes of
	// database fields nobody may fill at will, and characters no keyboard has. (A zero
	// in a name used to end the name in the database but not in the server's memory.)
	if( !VerifyUserData() )
	{
		return std::make_shared< ResultCreateAccount >( this, ERROR_FATAL, L"" );
	}

	if( !LobbyServer::Get().AllowAccountCreation( socket ) )
	{
		Log::Error( "RequestCreateAccount::ProcessRequest() - Too many accounts created from {}, refused.", socket->remote_ip );
		return std::make_shared< ResultCreateAccount >( this, ERROR_FATAL, L"" );
	}

	auto result = Database::Get().CreateNewAccount
	(
		Util::WideToUTF8( m_username ),
		Util::WideToUTF8( m_password ),
		Util::WideToUTF8( m_emailAddress ),
		Util::WideToUTF8( m_dateOfBirth ),
		Util::WideToUTF8( m_chatHandle )
	);

	if( !result )
	{
		Log::Error( "RequestCreateAccount::ProcessRequest() - Failed to create account for user: {}", m_username );
		return std::make_shared< ResultCreateAccount >( this, ERROR_FATAL, L"" );
	}

	// The connection is logged in as the new account from here on; a session it had
	// before is over.
	if( user->m_isLoggedIn )
	{
		UserManager::Get().EndSession( user );
	}

	user->m_isLoggedIn = true;
	user->m_sessionId = UserManager::Get().GenerateSessionId();
	user->m_accountId = result;
	user->m_username = m_username;
	user->m_chatHandle = m_chatHandle;

	return std::make_shared< ResultCreateAccount >( this, SUCCESS, user->m_sessionId );
}

ResultCreateAccount::ResultCreateAccount( GenericRequest *request, int32_t reply, std::wstring sessionId ) : GenericResponse( *request )
{
	m_reply = reply;
	m_sessionId = sessionId;
}

void ResultCreateAccount::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );

	out.write_encrypted_utf16( m_sessionId );
}
