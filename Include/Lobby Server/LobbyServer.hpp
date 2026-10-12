#pragma once

#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <array>
#include <cstdint>
#include <unordered_map>

#include "Platform.hpp"

#include "Common/Constant.hpp"
#include "Common/ByteStream.hpp"
#include "Common/RateLimit.hpp"
#include "../Network/RealmSocket.hpp"

class LobbyServer
{
private:
	std::atomic< bool > m_running;
	std::thread m_thread;

public:
	static LobbyServer& Get()
	{
		static LobbyServer instance;
		return instance;
	}

	LobbyServer( const LobbyServer & ) = delete;
	LobbyServer &operator=( const LobbyServer & ) = delete;
	LobbyServer();
	~LobbyServer();

	void Start( std::string ip );
	void Stop();
	bool isRunning() const
	{
		return m_running;
	}

	// Users, games and chat rooms are used by the lobby thread and by the discovery
	// thread. Whichever of them is processing a message holds this lock meanwhile.
	static std::mutex &StateMutex()
	{
		return m_stateMutex;
	}

	// The request being handled turned out to cost the server more than its message id
	// says (a character was written to the database). Charged to the sender's address
	// when the handler returns. Lobby thread only.
	void AddRequestCost( int32_t units )
	{
		m_extraCost += units;
	}

	// May the address of this connection create one more account now? Takes one from
	// its allowance when it may. Lobby thread only.
	bool AllowAccountCreation( const sptr_socket &socket );

private:
	static inline std::mutex m_stateMutex;

	// A client holding more unsent replies than this is dropped. The most the server
	// sends for one request is a character, 20 KB in 1 KB pieces.
	static constexpr size_t MAX_PENDING_WRITE_BYTES = 256 * 1024;

	// A connection the server has decided to close is given this long to take the
	// replies still waiting for it.
	static constexpr int32_t MAX_CLOSE_WAIT_SECONDS = 5;

	// Password checks (login, account details) per address: this many at once, and
	// one more every so many seconds. Account creations per address likewise.
	static constexpr double PASSWORD_CHECK_BURST = 40.0;
	static constexpr double PASSWORD_CHECK_SECONDS = 3.0;
	static constexpr double ACCOUNT_CREATION_BURST = 20.0;
	static constexpr double ACCOUNT_CREATION_SECONDS = 180.0;

	// What the server keeps per client address (IPv4, host byte order).
	struct AddressState
	{
		int32_t m_connections = 0;
		TokenBucket m_requests;
		TokenBucket m_passwordChecks;
		TokenBucket m_accountCreations;
		std::chrono::steady_clock::time_point m_lastSeen{};
	};

	std::unordered_map< uint32_t, AddressState > m_addresses;
	int32_t m_extraCost = 0;

	// Two passes of the lobby loop are a millisecond or so apart. When they are further
	// apart than this, the server itself stood still (the host computer was asleep, the
	// process was stopped) and that time is not counted as silence of the clients.
	static constexpr int32_t MAX_SERVER_PAUSE_SECONDS = 5;

	sptr_socket m_conSocket;
	sptr_socket m_rtaSocket;
	std::vector< sptr_socket > m_gatewaySockets;
	std::vector< sptr_socket > m_clientSockets;

	std::vector< uint8_t > m_recvBuffer;

	// When CheckSocketProblem last ran.
	std::chrono::steady_clock::time_point m_lastSocketCheck;

	sptr_socket OpenListenerSocket( std::string ip, int32_t port, RealmGameType type );

	void Run();
	void CheckSocketProblem();
	void AcceptConnection( sptr_socket srcSocket );
	void ReadSocket( sptr_socket socket );
	void ProcessFrames( sptr_socket socket );
	void DropSocket( sptr_socket socket );
	bool MakeRoomForConnection();
	void ForgetIdleAddresses();
	static double RequestCost( uint16_t packetId );
	void WriteSocket( sptr_socket socket );
	void HandleRequest( sptr_socket socket, sptr_byte_stream stream );
	void ResumeSession( sptr_socket socket, sptr_byte_stream stream, uint16_t packetId );
};