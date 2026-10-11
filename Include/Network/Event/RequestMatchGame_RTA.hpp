#pragma once

#include <memory>
#include <string>

#include "../GenericNetRequest.hpp"
#include "../GenericNetResponse.hpp"

class RequestMatchGame_RTA : public GenericRequest
{
private:
	enum REPLY {
		SUCCESS = 0,
		INVALID_SESSION = 7,
	};

public:
	static std::unique_ptr< RequestMatchGame_RTA > Create()
	{
		return std::make_unique< RequestMatchGame_RTA >();
	}

	sptr_generic_response ProcessRequest( sptr_socket socket, sptr_byte_stream stream ) override;
	void Deserialize( sptr_byte_stream stream ) override;
};

class ResultMatchGame_RTA : public GenericResponse {
private:
	int32_t m_reply;
	std::string m_userIp;

public:
	ResultMatchGame_RTA( GenericRequest *request, int32_t reply, std::string userIp );
	void Serialize( ByteBuffer &out ) const;
};