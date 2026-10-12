#include "Network/Event/RequestLogin.hpp"

#include "Database/Database.hpp"
#include "Game/RealmUserManager.hpp"
#include "Game/RealmUser.hpp"
#include "Common/Constant.hpp"
#include "logging.hpp"

void RequestLogin::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	m_username = stream->read_encrypted_utf16();
	m_password = stream->read_encrypted_utf16();
}

sptr_generic_response RequestLogin::ProcessLoginCON( sptr_user user )
{
	if( m_username != L"foo" && m_password != L"bar" )
	{
		// Network Beta CoN uses login information, but it's invalid because of version 2.0
		// which used "foo" and "bar" as the login credentials.
		Log::Debug( "RequestLogin : Champions of Norrath v1.0" );

		// TODO: Either block this, or add support for the network beta.
		return std::make_shared< ResultLogin >( this, LOGIN_REPLY::ACCOUNT_INVALID, L"" );
	}

	// A connection that logs in a second time starts afresh.
	if( user->m_isLoggedIn )
	{
		UserManager::Get().EndSession( user );
	}

	user->m_isLoggedIn = true;
	user->m_accountId = -1;
	user->m_sessionId = UserManager::Get().GenerateSessionId();

	return std::make_shared< ResultLogin >( this, SUCCESS, user->m_sessionId );
}

sptr_generic_response RequestLogin::ProcessLoginRTA( sptr_user user )
{
	// Return to Arms uses login information.
	Log::Debug( "RequestLogin : Return to Arms" );

	auto &UserManager = UserManager::Get();
	auto &Database = Database::Get();

	// Verify the account exists
	auto [ result, accountId, chatHandle ] = Database.VerifyAccount( m_username, m_password );

	if( accountId < 0 )
	{
		Log::Error( "RequestLogin::ProcessRequest() - Invalid account ID: " + std::to_string( accountId ) );
		return std::make_shared< ResultLogin >( this, ACCOUNT_INVALID, L"" );
	}

	// Check if the user is already logged in
	for( const auto &existingUser : UserManager.GetUserList() )
	{
		// Logging in again on the same connection is not a second login.
		if( existingUser == user )
		{
			continue;
		}

		if( existingUser->m_username == m_username || existingUser->m_accountId == accountId )
		{
			// The other connection may belong to a console that was switched off without
			// closing it, and would otherwise lock the account until it times out. The
			// newcomer has just proved it knows the password, so it takes over.
			Log::Info( "[{}] logged in again, closing the earlier session", m_username );
			UserManager.Disconnect( existingUser, "Logged in from another connection" );
		}
	}

	// The same goes for a session whose connection was lost a moment ago and which
	// is waiting to be picked up again: this login replaces it.
	UserManager.ForgetLostSessions( accountId );

	// A connection that is logged in and logs in again starts afresh: the game, the
	// chat rooms and the unfinished upload of the session it had are given up.
	if( user->m_isLoggedIn )
	{
		UserManager.EndSession( user );
	}

	// Login Success
	user->m_isLoggedIn = true;
	user->m_username = m_username;
	user->m_accountId = accountId;
	user->m_chatHandle = chatHandle;
	user->m_sessionId = UserManager.GenerateSessionId();

	// Load Friend List
	user->m_friendList = Database.LoadFriends( accountId );

	// Load Ignore List
	user->m_ignoreList = Database.LoadIgnores( accountId );

	// Notify friends about the user's online status
	UserManager.NotifyFriendsOnlineStatus( user, true );

	// The game keeps the second string of the reply as its own chat handle.
	return std::make_shared< ResultLogin >( this, SUCCESS, user->m_sessionId, user->m_chatHandle );
}

sptr_generic_response RequestLogin::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindUserBySocket( socket );
	if( user == nullptr )
	{
		Log::Error( "RequestLogin::ProcessRequest() - User not found" );
		return std::make_shared< ResultLogin >( this, ACCOUNT_INVALID, L"" );
	}

	if( m_username.empty() || m_password.empty() )
	{
		Log::Error( "RequestLogin::ProcessRequest() - Username or password is empty" );
		return std::make_shared< ResultLogin >( this, ACCOUNT_INVALID, L"" );
	}

	if( user->m_gameType == RealmGameType::CHAMPIONS_OF_NORRATH )
	{
		return ProcessLoginCON( user );
	}
	else
	{
		return ProcessLoginRTA( user );
	}
}

ResultLogin::ResultLogin( GenericRequest *request, int32_t reply, std::wstring sessionId, std::wstring chatHandle ) : GenericResponse( *request )
{
	m_reply = reply;
	m_sessionId = sessionId;
	m_chatHandle = chatHandle;
}

void ResultLogin::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );

	out.write_encrypted_utf16( m_sessionId );
	out.write_encrypted_utf16( m_chatHandle );
}
