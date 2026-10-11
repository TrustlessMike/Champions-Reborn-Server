#pragma once

#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <array>

#include "Platform.hpp"

#include "Common/Constant.hpp"
#include "Common/ByteStream.hpp"
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

private:
	static inline std::mutex m_stateMutex;

	// A client holding more unsent replies than this is dropped.
	static constexpr size_t MAX_PENDING_WRITE_BYTES = 1024 * 1024;

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
	void WriteSocket( sptr_socket socket );
	void HandleRequest( sptr_socket socket, sptr_byte_stream stream );
	void ResumeSession( sptr_socket socket, sptr_byte_stream stream, uint16_t packetId );
};