#pragma once

#include <cstddef>
#include <cstdint>

#include <fstream>
#include <string>
#include <vector>

class Config
{
public:
	static bool Load( std::string filename );

	static inline std::string service_ip;
	static inline std::string public_ip;
	static inline uint16_t con_lobby_port;
	static inline uint16_t rta_lobby_port;
	static inline uint16_t discovery_port;

	// First of the eight gateway ports of each game. The games have 40800 (Champions of
	// Norrath) and 40810 (Return to Arms) compiled in, so only a second instance for
	// testing, beside one that is running, has a reason to change them.
	static inline uint16_t con_gateway_port;
	static inline uint16_t rta_gateway_port;

	// Seconds a connection may stay silent before it is dropped. A session whose
	// connection was lost can be picked up again for as long (UserManager::ResumeSession).
	static inline int32_t client_timeout;

	// Limits against abuse (FIXES.md 21 onwards, SECURITY.md). A console never comes
	// near any of them.
	//
	// Client connections the server holds at once, in all and from one address. When
	// the first is reached, a newcomer takes the place of the connection that has not
	// logged in and has been silent the longest.
	static inline int32_t max_connections;
	static inline int32_t max_connections_per_address;

	// Seconds a new connection has to send its first complete message.
	static inline int32_t first_message_timeout;

	// Requests are paid for from a budget per address: this many units are added each
	// second, up to request_burst. A plain request costs one unit, one that makes the
	// server work (a password check, a database write) more. A connection whose address
	// has used its budget up is not read until there is budget again; nothing is
	// refused or dropped.
	static inline int32_t requests_per_second;
	static inline int32_t request_burst;

	// The limits per address (connections, request budget, password checks, accounts)
	// are for strangers. Addresses that only the server's own machine and its own
	// network can have (127.0.0.0/8, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16) are
	// left alone unless this is set: a test rig there floods on purpose. Set it when
	// strangers reach the server through something that gives them such an address.
	static inline bool limits_for_local_addresses;

	// Addresses the limits per address leave alone as well, whatever the setting above
	// says (IPv4, host byte order; in the file: dotted, separated by commas or spaces,
	// 16 at most, no ranges). For a relay that speaks for many players and reaches the
	// server from one address that is not a private one, such as the server's own
	// public address. Everything that connects from such an address shares no limit at
	// all, so whatever stands there has to do the limiting itself (SECURITY.md). Empty
	// unless set.
	static constexpr size_t MAX_TRUSTED_ADDRESSES = 16;
	static inline std::vector< uint32_t > trusted_addresses;

	// Do the limits per address leave a client at this address alone?
	static bool ExemptFromAddressLimits( const std::string &ip, uint32_t addressKey );

	// The database file is not allowed to grow beyond this many megabytes.
	static inline int32_t max_database_mb;

	// When set, a console the server observes at a private or loopback address is
	// reported to players outside that network as being at public_ip. Off by default.
	static inline bool report_public_ip_for_lan_consoles;

	// The address of a console as it should be told to the player at recipientIp.
	static std::string ReportedAddress( const std::string &observedIp, const std::string &recipientIp );

	// When set, a Return to Arms console is told the address the server observed it at
	// as its own (0x3E, 0x40), as this server always did. Off by default: it is told
	// 0.0.0.0, see OwnAddressForRta.
	static inline bool rta_tell_console_its_observed_address;

	// What a Return to Arms console is told as its own address in 0x3E and 0x40.
	static std::string OwnAddressForRta( const std::string &observedIp );
};