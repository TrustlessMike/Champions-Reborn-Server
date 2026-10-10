#pragma once

#include <memory>
#include <string>

#include "../GenericNetRequest.hpp"
#include "../GenericNetResponse.hpp"

#include "Game/ChatRoomSession.hpp"

class RequestCreatePrivateRoom : public GenericRequest
{
private:
	std::wstring m_sessionId;
	std::wstring m_roomName;

	enum REPLY {
		SUCCESS = 0,
		GENERAL_ERROR
	};

public:
	static std::unique_ptr< RequestCreatePrivateRoom > Create()
	{
		return std::make_unique< RequestCreatePrivateRoom >();
	}

	sptr_generic_response ProcessRequest( sptr_socket socket, sptr_byte_stream stream ) override;
	void Deserialize( sptr_byte_stream stream ) override;
};

class ResultCreatePrivateRoom : public GenericResponse {
private:
	int32_t m_reply;
	sptr_chat_room_session m_room;

public:
	ResultCreatePrivateRoom( GenericRequest *request, int32_t reply, sptr_chat_room_session room );
	void Serialize( ByteBuffer &out ) const;
};