#include "Network/Event/NotifyForcedLogout.hpp"

NotifyForcedLogout::NotifyForcedLogout() : GenericMessage( 0x41 )
{
}

void NotifyForcedLogout::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( 0 );

	// The game reads one 16-bit number after the header and passes it to a handler
	// that does not look at it.
	out.write_u16( 0 );
}
