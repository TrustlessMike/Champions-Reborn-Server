#include "Network/Event/NotifyGameDiscovered.hpp"
#include "Game/RealmUser.hpp"
#include "configuration.hpp"

NotifyGameDiscovered::NotifyGameDiscovered( sptr_user user ) : GenericMessage( 0x3E )
{
	this->m_clientIp = user->m_discoveryAddr;
	this->m_clientPort = user->m_discoveryPort;
	this->m_clientType = user->m_gameType;

	// Return to Arms must not be told the address it was observed at (Config::OwnAddressForRta).
	if( m_clientType == RealmGameType::RETURN_TO_ARMS )
		this->m_clientIp = Config::OwnAddressForRta( user->m_discoveryAddr );
}

void NotifyGameDiscovered::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( 0 );

	if( m_clientType == RealmGameType::RETURN_TO_ARMS )
		out.write_utf8( m_clientIp );
	else
		out.write_sz_utf8( m_clientIp );

	out.write_u32( m_clientPort );
}
