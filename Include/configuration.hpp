#pragma once

#include <fstream>
#include <string>

class Config
{
public:
	static bool Load( std::string filename );

	static inline std::string service_ip;
	static inline std::string public_ip;
	static inline uint16_t con_lobby_port;
	static inline uint16_t rta_lobby_port;
	static inline uint16_t discovery_port;

	// Seconds a connection may stay silent before it is dropped.
	static inline int32_t client_timeout;

	// When set, a console the server observes at a private or loopback address is
	// reported to players outside that network as being at public_ip. Off by default.
	static inline bool report_public_ip_for_lan_consoles;

	// The address of a console as it should be told to the player at recipientIp.
	static std::string ReportedAddress( const std::string &observedIp, const std::string &recipientIp );
};