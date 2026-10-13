#include "Network/Event/NotifyClientDiscovered_RTA.hpp"

#include "Common/Constant.hpp"
#include "Game/RealmUser.hpp"
#include "configuration.hpp"

NotifyClientDiscovered_RTA::NotifyClientDiscovered_RTA( sptr_user user ) : GenericMessage( 0x40 )
{
	// Not the address the joiner was observed at (Config::OwnAddressForRta).
	this->m_clientIp = Config::OwnAddressForRta( user->m_discoveryAddr );
	this->m_clientPort = user->m_discoveryPort;
}

void  NotifyClientDiscovered_RTA::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( 0 );

	out.write_utf8( m_clientIp );
	out.write_u32( m_clientPort );
}
