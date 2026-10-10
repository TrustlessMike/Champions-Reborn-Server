#pragma once

#include <memory>
#include <string>

#include "../GenericNetRequest.hpp"
#include "../GenericNetResponse.hpp"

class RequestLogin : public GenericRequest
{
private:
	enum LOGIN_REPLY {
		SUCCESS = 0,
		FATAL_ERROR,
		ACCOUNT_INVALID = 4,
	};

	std::wstring m_username;
	std::wstring m_password;
	std::wstring m_sessionId;

public:
	static std::unique_ptr< RequestLogin > Create()
	{
		return std::make_unique< RequestLogin >();
	}

	sptr_generic_response ProcessRequest( sptr_socket socket, sptr_byte_stream stream ) override;
	void Deserialize( sptr_byte_stream stream ) override;

	sptr_generic_response ProcessLoginCON( sptr_user user );
	sptr_generic_response ProcessLoginRTA( sptr_user user );
};

class ResultLogin : public GenericResponse {
private:
	std::wstring m_sessionId;
	std::wstring m_chatHandle;
	int32_t m_reply;

public:
	// Champions of Norrath has no chat handles; it keeps getting the placeholder.
	ResultLogin( GenericRequest *request, int32_t reply, std::wstring sessionId, std::wstring chatHandle = L"UNKNOWN DUMMY STRING" );
	void Serialize( ByteBuffer &out ) const;
};