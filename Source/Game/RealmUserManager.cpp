#include "Game/RealmUserManager.hpp"

#include <algorithm>

#include "Game/GameSessionManager.hpp"
#include "Game/ChatRoomManager.hpp"
#include "Game/CharacterSaveManager.hpp"

#include "Network/Event/NotifyForcedLogout.hpp"
#include "Network/Event/NotifyFriendStatus.hpp"
#include "Database/Database.hpp"
#include "Common/Constant.hpp"
#include "configuration.hpp"
#include "logging.hpp"

UserManager::UserManager()
{
	std::random_device rd;
	rng.seed( rd() );

	m_users.clear();
}

UserManager::~UserManager()
{
}

std::wstring UserManager::GenerateSessionId()
{
	static const wchar_t charset[] = L"0123456789ABCDEF";
	std::uniform_int_distribution<int> dist( 0, 15 );

	std::wstring sessionId;
	sessionId.reserve( MAX_SESSION_ID_LENGTH );

	for( int i = 0; i < MAX_SESSION_ID_LENGTH; ++i )
	{
		sessionId += charset[ dist( rng ) ];
	}

	return sessionId;
}

sptr_user UserManager::CreateUser( sptr_socket socket, RealmGameType clientType )
{
	Log::Debug( "ClientManager::CreateUser() - Created new user" );

	auto user = std::make_shared< RealmUser >();

	user->sock = socket;
	user->m_gameType = clientType;

	std::lock_guard< std::mutex > lock( m_mutex );
	m_users.push_back( user );

	return user;
}

void UserManager::RemoveUser( sptr_user user )
{
	auto it = std::find( m_users.begin(), m_users.end(), user );
	if( it == m_users.end() )
	{
		Log::Error( "RemoveUser : [{}] not found", user->m_sessionId );
		return;
	}

	GameSessionManager::Get().OnDisconnectUser( user );
	ChatRoomManager::Get().OnDisconnectUser( user );

	// An upload this user had begun will never be finished, and its task would
	// keep the user and its socket alive.
	if( !user->m_sessionId.empty() )
	{
		CharacterSaveManager::Get().m_tasks.erase( user->m_sessionId );
	}

	NotifyFriendsOnlineStatus( user, false );

	Log::Debug( "RemoveUser : [{}][{}]", user->m_username, user->m_sessionId );

	std::lock_guard< std::mutex > lock( m_mutex );
	m_users.erase( it );
}

void UserManager::RemoveUser( const std::wstring &sessionId )
{
	if( auto user = FindUserBySessionId( sessionId ) )
	{
		RemoveUser( user );
	}
}

void UserManager::RemoveUser( const sptr_socket socket )
{
	if( auto user = FindUserBySocket( socket ) )
	{
		RemoveUser( user );
	}
}

void UserManager::Disconnect( sptr_socket socket, const std::string reason )
{
	if( nullptr == socket )
	{
		return;
	}

	Log::Debug( "DisconnectSocket : [{}]. Reason: {}", socket->remote_ip, reason );

	socket->send( NotifyForcedLogout() );
	socket->flag.disconnected_wait = true;

	RemoveUser( socket );
}

void UserManager::Disconnect( sptr_user user, const std::string reason )
{
	if( nullptr == user )
	{
		return;
	}

	if( user->sock != nullptr )
	{
		user->sock->send( NotifyForcedLogout() );
		user->sock->flag.disconnected_wait = true;
	}

	Log::Debug( "DisconnectUser : [{}]. Reason: {}", user->m_sessionId, reason );

	RemoveUser( user );
}

void UserManager::OnConnectionLost( const sptr_socket socket )
{
	auto user = FindUserBySocket( socket );
	if( user == nullptr )
	{
		return;
	}

	// Champions of Norrath has no accounts, and every console of it logs in alike.
	if( user->m_isLoggedIn && user->m_gameType == RealmGameType::RETURN_TO_ARMS && user->m_accountId > 0 && !user->m_sessionId.empty() )
	{
		LostSession lost;

		lost.m_sessionId = user->m_sessionId;
		lost.m_remoteIp = socket->remote_ip;
		lost.m_accountId = user->m_accountId;
		lost.m_username = user->m_username;
		lost.m_chatHandle = user->m_chatHandle;
		lost.m_characterId = user->m_characterId;
		lost.m_character = user->m_character;
		lost.m_lostAt = std::chrono::steady_clock::now();

		std::lock_guard< std::mutex > lock( m_mutex );

		// An account has one session at a time, and a lost one is kept only as long
		// as a silent connection would have been.
		m_lostSessions.erase( std::remove_if( m_lostSessions.begin(), m_lostSessions.end(), [ & ]( const LostSession &other )
		{
			return other.m_accountId == lost.m_accountId
				|| lost.m_lostAt - other.m_lostAt > std::chrono::seconds( Config::client_timeout );
		} ), m_lostSessions.end() );

		m_lostSessions.push_back( lost );
	}

	RemoveUser( user );
}

void UserManager::ForgetLostSessions( int64_t accountId )
{
	std::lock_guard< std::mutex > lock( m_mutex );

	m_lostSessions.erase( std::remove_if( m_lostSessions.begin(), m_lostSessions.end(), [ & ]( const LostSession &lost )
	{
		return lost.m_accountId == accountId;
	} ), m_lostSessions.end() );
}

bool UserManager::ResumeSession( const sptr_user &user, const std::wstring &sessionId )
{
	if( !user || user->m_isLoggedIn || user->sock == nullptr || user->m_gameType != RealmGameType::RETURN_TO_ARMS )
	{
		return false;
	}

	if( sessionId.length() != MAX_SESSION_ID_LENGTH )
	{
		return false;
	}

	LostSession lost;

	{
		std::lock_guard< std::mutex > lock( m_mutex );

		const auto now = std::chrono::steady_clock::now();
		m_lostSessions.erase( std::remove_if( m_lostSessions.begin(), m_lostSessions.end(), [ & ]( const LostSession &other )
		{
			return now - other.m_lostAt > std::chrono::seconds( Config::client_timeout );
		} ), m_lostSessions.end() );

		// Only a session whose connection is gone can be picked up. One that a
		// connection still holds stays with that connection.
		auto it = std::find_if( m_lostSessions.begin(), m_lostSessions.end(), [ & ]( const LostSession &other )
		{
			return other.m_sessionId == sessionId;
		} );

		if( it == m_lostSessions.end() )
		{
			return false;
		}

		// The id alone proves little: it travels under a key every console has, and
		// the players of a game are told each other's. What the server knows besides
		// is the address the session was used from, so it can only be picked up from
		// there - the condition a discovery datagram has to meet as well.
		if( it->m_remoteIp != user->sock->remote_ip )
		{
			Log::Error( "Session of [{}] presented from another address ({}), refused", it->m_username, user->sock->remote_ip );
			return false;
		}

		// Picked up once; from here on it is this connection's session.
		lost = *it;
		m_lostSessions.erase( it );
	}

	user->m_isLoggedIn = true;
	user->m_sessionId = lost.m_sessionId;
	user->m_accountId = lost.m_accountId;
	user->m_username = lost.m_username;
	user->m_chatHandle = lost.m_chatHandle;
	user->m_characterId = lost.m_characterId;
	user->m_character = lost.m_character;

	// The rest is what a login does.
	user->m_friendList = Database::Get().LoadFriends( lost.m_accountId );
	user->m_ignoreList = Database::Get().LoadIgnores( lost.m_accountId );

	NotifyFriendsOnlineStatus( user, true );

	Log::Info( "[{}] picked its session up again on a new connection", user->m_username );

	return true;
}

sptr_user UserManager::FindUserBySessionId( const std::wstring &sessionId )
{
	std::lock_guard<std::mutex> lock( m_mutex );
	auto it = std::find_if( m_users.begin(), m_users.end(), [ & ]( const sptr_user &user )
	{
		return user->m_sessionId == sessionId;
	} );
	return ( it != m_users.end() ) ? *it : nullptr;
}

sptr_user UserManager::FindUserBySocket( const sptr_socket &socket )
{
	std::lock_guard<std::mutex> lock( m_mutex );
	auto it = std::find_if( m_users.begin(), m_users.end(), [ & ]( const sptr_user &user )
	{
		return user->sock == socket;
	} );
	return ( it != m_users.end() ) ? *it : nullptr;
}

sptr_user UserManager::FindUserByChatHandle( const std::wstring &handle )
{
	std::lock_guard<std::mutex> lock( m_mutex );
	auto it = std::find_if( m_users.begin(), m_users.end(), [ & ]( const sptr_user &user )
	{
		return user->m_chatHandle == handle;
	} );
	return ( it != m_users.end() ) ? *it : nullptr;
}

int32_t UserManager::GetUserCount() const
{
	return static_cast< int32_t >( m_users.size() );
}

std::vector<sptr_user> UserManager::GetUserList()
{
	std::lock_guard<std::mutex> lock( m_mutex );
	return m_users;
}

void UserManager::NotifyFriendsOnlineStatus( const sptr_user &user, bool onlineStatus )
{
	if( !user || user->m_friendList.empty() )
	{
		return;
	}

	const auto notifyFriend = NotifyFriendStatus( user->m_chatHandle, onlineStatus );
	for( const auto &friendHandle : user->m_friendList )
	{
		auto friendUser = FindUserByChatHandle( friendHandle );
		if( friendUser && friendUser->sock )
		{
			friendUser->sock->send( notifyFriend );
		}
	}
}

std::vector< std::wstring > UserManager::GetOnlineFriends( const sptr_user &user )
{
	std::vector< std::wstring > online;

	if( !user )
	{
		return online;
	}

	for( const auto &friendHandle : user->m_friendList )
	{
		auto friendUser = FindUserByChatHandle( friendHandle );
		if( friendUser && friendUser->m_isLoggedIn )
		{
			online.push_back( friendHandle );
		}
	}

	return online;
}
