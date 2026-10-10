#pragma once

#include <memory>
#include <string>

#include "../GenericNetRequest.hpp"
#include "../GenericNetResponse.hpp"

// 0x19. The game sends it once its game's chat room exists (after 0x09), with a
// line of text for the room. Whoever enters the room is given that text.
class RequestSetRoomBanner : public GenericRequest
{
private:
	std::wstring m_sessionId;
	std::wstring m_roomName;
	std::wstring m_banner;

	enum REPLY {
		SUCCESS = 0,
		GENERAL_ERROR
	};

public:
	static std::unique_ptr< RequestSetRoomBanner > Create()
	{
		return std::make_unique< RequestSetRoomBanner >();
	}

	sptr_generic_response ProcessRequest( sptr_socket socket, sptr_byte_stream stream ) override;
	void Deserialize( sptr_byte_stream stream ) override;
};

class ResultSetRoomBanner : public GenericResponse {
private:
	int32_t m_reply;

public:
	ResultSetRoomBanner( GenericRequest *request, int32_t reply );
	void Serialize( ByteBuffer &out ) const;
};
