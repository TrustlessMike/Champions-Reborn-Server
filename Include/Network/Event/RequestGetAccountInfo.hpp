#pragma once

#include <array>
#include <memory>
#include <string>

#include "../GenericNetRequest.hpp"
#include "../GenericNetResponse.hpp"

// 0x64. The game sends it with the name and password typed on the login screen when
// a login attempt came back with result 63, and fills its account screen from the
// reply: twelve encrypted strings, of which it uses the first (e-mail address) and
// the sixth (date of birth). This server never answers a login with 63, so a game
// has no reason to ask; the request is answered all the same.
class RequestGetAccountInfo : public GenericRequest
{
private:
	enum REPLY {
		SUCCESS = 0,
		ACCOUNT_INVALID = 4,
	};

	std::wstring m_username;
	std::wstring m_password;

public:
	static std::unique_ptr< RequestGetAccountInfo > Create()
	{
		return std::make_unique< RequestGetAccountInfo >();
	}

	sptr_generic_response ProcessRequest( sptr_socket socket, sptr_byte_stream stream ) override;
	void Deserialize( sptr_byte_stream stream ) override;
};

class ResultGetAccountInfo : public GenericResponse {
private:
	int32_t m_reply;
	std::array< std::wstring, 12 > m_fields;

public:
	ResultGetAccountInfo( GenericRequest *request, int32_t reply, std::wstring emailAddress = L"", std::wstring dateOfBirth = L"" );
	void Serialize( ByteBuffer &out ) const;
};
