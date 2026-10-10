#pragma once

#include <memory>
#include <string>

#include "../GenericNetRequest.hpp"
#include "../GenericNetResponse.hpp"
#include "Common/ForwardDecl.hpp"

// 0x63. Sent by the game's ignore list screen; the reply is the list of ignored
// chat handles. (0x62, RequestGetSocialListUpdate, is its counterpart for friends.)
class RequestGetIgnoreList : public GenericRequest
{
public:
	static std::unique_ptr< RequestGetIgnoreList > Create()
	{
		return std::make_unique< RequestGetIgnoreList >();
	}

	sptr_generic_response ProcessRequest( sptr_socket socket, sptr_byte_stream stream ) override;
	void Deserialize( sptr_byte_stream stream ) override;
};

class ResultGetIgnoreList : public GenericResponse {
private:
	sptr_user m_user;

public:
	ResultGetIgnoreList( GenericRequest *request, sptr_user user = nullptr );
	void Serialize( ByteBuffer &out ) const;
};
