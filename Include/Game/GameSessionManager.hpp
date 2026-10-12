#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <mutex>
#include <vector>

#include "GameSession.hpp"

#include "Common/Constant.hpp"

class GameSessionManager {
private:
	static inline std::unique_ptr< GameSessionManager > m_instance;
	static inline std::mutex m_mutex;
	static inline std::mutex m_dataMutex;

	int32_t m_uniqueGameIndex;
	std::vector< sptr_game_session > m_gameSessionList[ 2 ];

public:
	GameSessionManager();
	~GameSessionManager();
	GameSessionManager( const GameSessionManager & ) = delete;
	GameSessionManager &operator=( const GameSessionManager & ) = delete;

	static GameSessionManager &Get()
	{
		std::lock_guard< std::mutex > lock( m_mutex );
		if( m_instance == nullptr )
		{
			m_instance.reset( new GameSessionManager() );
		}

		return *m_instance;
	}

	void OnDisconnectUser( sptr_user user );

	// Takes the user out of every game but the one named (-1 for none): its own
	// games are removed, its places in others given back.
	void LeaveGames( const sptr_user &user, const int32_t exceptGameId );

	// Games the server lists at once, per game type, and the longest name or info
	// string (the game's own list takes 256 characters of info).
	static constexpr size_t MAX_GAMES = 256;
	static constexpr size_t MAX_GAME_NAME_LENGTH = 256;

	bool CreateGameSession_CON( sptr_user user,
								const std::wstring gameInfo,
								const std::wstring name,
								const std::wstring stage,
								const bool isPrivateGame );

	bool CreateGameSession_RTA( sptr_user user,
								const std::wstring gameInfo,
								const std::wstring name,
								const std::array< int8_t, 5 > &attributes,
								const bool isPrivateGame );

	bool ForceTerminateGame( const int32_t gameId, RealmGameType clientType );
	sptr_game_session FindGame( const int32_t gameId, RealmGameType clientType );
	sptr_game_session FindGame( const std::wstring &gameName, RealmGameType clientType );

	bool RequestOpen( sptr_user user );
	bool RequestCancel( sptr_user user );
	bool RequestJoin( sptr_user user );
	bool RequestStart( sptr_user user );

	std::vector< sptr_game_session > GetAvailableGameSessionList( const RealmGameType clientType ) const;
	std::vector< sptr_game_session > GetPublicGameSessionList( const RealmGameType clientType ) const;
	std::vector< sptr_game_session > GetPrivateGameSessionList( const RealmGameType clientType ) const;

private:
	bool MayCreateGame( const std::wstring &gameInfo, const std::wstring &name, const RealmGameType gameType );
	void ProcessJoinNorrath( sptr_user join, sptr_user host );
	void ProcessJoinArms( sptr_user join, sptr_user host );
};