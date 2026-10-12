#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <random>

#include "RealmUser.hpp"

class UserManager {
private:

public:
	static UserManager &Get()
	{
		static UserManager instance;
		return instance;
	}

	UserManager( const UserManager & ) = delete;
	UserManager &operator=( const UserManager & ) = delete;
	UserManager();
	~UserManager();

	std::wstring GenerateSessionId();
	sptr_user CreateUser( sptr_socket socket, RealmGameType clientType );
	void RemoveUser( sptr_user user );
	void RemoveUser( const std::wstring &sessionId );
	void RemoveUser( const sptr_socket socket );
	void Disconnect( sptr_socket socket, const std::string reason );
	void Disconnect( sptr_user user, const std::string reason );

	// The connection of this socket's user has ended without a logout. The user is
	// removed like any other, but its session can be picked up again for a while.
	void OnConnectionLost( const sptr_socket socket );

	// A connection that is not logged in sent a request under this session id. When
	// the id is that of a lost session and comes from the address the session was
	// used from, the connection's user becomes that session's user again, in the
	// public chat room it was in. Any other id changes nothing and returns false.
	bool ResumeSession( const sptr_user &user, const std::wstring &sessionId );

	// The account has logged in anew: no earlier session of it may be picked up.
	void ForgetLostSessions( int64_t accountId );

	sptr_user FindUserBySessionId( const std::wstring &sessionId );
	sptr_user FindUserBySocket( const sptr_socket &socket );

	// The user of this connection if it has logged in, otherwise nothing. What a
	// request handler asks: a connection that has not logged in is to it what a
	// connection without a user is.
	sptr_user FindLoggedInUser( const sptr_socket &socket );

	// Ends what the user's session holds (games, chat rooms, an upload, its friends'
	// view of it) and leaves the user as it was before it logged in.
	void EndSession( const sptr_user &user );
	sptr_user FindUserByChatHandle( const std::wstring &handle );
	int32_t GetUserCount() const;
	std::vector< sptr_user > GetUserList();

	void NotifyFriendsOnlineStatus( const sptr_user &user, bool onlineStatus );

	// Those of the user's friends who are logged in right now.
	std::vector< std::wstring > GetOnlineFriends( const sptr_user &user );

private:
	// What is kept of a logged-in user whose connection was lost.
	struct LostSession {
		std::wstring	m_sessionId;
		std::string		m_remoteIp;			// Address the lost connection came from
		int64_t			m_accountId;
		std::wstring	m_username;
		std::wstring	m_chatHandle;
		int32_t			m_characterId;		// The character a save would go to
		sptr_realm_character m_character;
		int32_t			m_publicRoomId;		// The public chat room it was in, or -1
		int32_t			m_partyGameId;		// The game it last opened or joined, or -1
		std::chrono::steady_clock::time_point m_lostAt;
	};

	std::mutex m_mutex;
	std::vector< sptr_user > m_users;
	std::vector< LostSession > m_lostSessions;
	std::random_device m_randomDevice;
};