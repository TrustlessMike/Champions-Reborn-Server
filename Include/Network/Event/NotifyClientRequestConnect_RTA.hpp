#pragma once

#include <memory>
#include <string>

#include "../GenericNetMessage.hpp"
#include "Common/ForwardDecl.hpp"

class NotifyClientRequestConnect_RTA : public GenericMessage {
private:
	std::string m_localAddr;
	std::string m_remoteAddr;
	int32_t m_localPort;
	int32_t m_remotePort;

public:
	// recipientIp is the address of the player being told, see Config::ReportedAddress.
	NotifyClientRequestConnect_RTA( sptr_user user, const std::string &recipientIp );
	void Serialize( ByteBuffer &out ) const override;
};