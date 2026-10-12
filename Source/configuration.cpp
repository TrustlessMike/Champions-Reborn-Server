#include "configuration.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "Platform.hpp"

#include "Common/Utility.hpp"
#include "logging.hpp"

// A number from the file. Anything that is not one leaves the default in place instead
// of ending the server with an exception.
static void ReadNumber( const std::string &key, const std::string &value, int64_t low, int64_t high, int64_t &out )
{
	try
	{
		size_t used = 0;
		const long long number = std::stoll( value, &used );

		if( used != value.size() || number < low || number > high )
		{
			throw std::out_of_range( "out of range" );
		}

		out = number;
	}
	catch( const std::exception & )
	{
		Log::Error( "config: {} = [{}] is not a number from {} to {}, keeping {}", key, value, low, high, out );
	}
}

template< typename T >
static void ReadNumber( const std::string &key, const std::string &value, int64_t low, int64_t high, T &out )
{
	int64_t number = out;
	ReadNumber( key, value, low, high, number );
	out = static_cast< T >( number );
}

static bool ReadSwitch( const std::string &value )
{
	return value == "1" || value == "true" || value == "on" || value == "yes";
}

bool Config::Load( std::string filename )
{
	service_ip = "0.0.0.0";
	public_ip = "";
	con_lobby_port = 40900;
	rta_lobby_port = 40910;
	discovery_port = 10101;
	con_gateway_port = 40800;
	rta_gateway_port = 40810;
	client_timeout = 120;
	report_public_ip_for_lan_consoles = false;
	max_connections = 512;
	max_connections_per_address = 16;
	first_message_timeout = 30;
	requests_per_second = 50;
	request_burst = 400;
	limits_for_local_addresses = false;
	max_database_mb = 1024;

	// Read configuration from ini file
	std::ifstream file( filename );
	if( !file.is_open() )
	{
		return false;
	}

	std::string line;

	while( std::getline( file, line ) )
	{
		if( line.empty() || line[0] == '#' || line[0] == ';' )
		{
			continue;
		}

		size_t pos = line.find( '=' );
		if( pos == std::string::npos )
		{
			continue;
		}

		std::string key = line.substr( 0, pos );
		std::string value = line.substr( pos + 1 );

		// A file saved with Windows line endings leaves a carriage return on every value.
		while( !value.empty() && ( value.back() == '\r' || value.back() == ' ' || value.back() == '\t' ) )
		{
			value.pop_back();
		}

		if( key == "service_ip" )
		{
			service_ip = value;
		}
		else if( key == "public_ip" )
		{
			public_ip = value;
		}
		else if( key == "con_lobby_port" )
		{
			ReadNumber( key, value, 1, 65535, con_lobby_port );
		}
		else if (key == "rta_lobby_port")
		{
			ReadNumber( key, value, 1, 65535, rta_lobby_port );
		}
		else if( key == "discovery_port" )
		{
			ReadNumber( key, value, 1, 65535, discovery_port );
		}
		else if( key == "con_gateway_port" )
		{
			ReadNumber( key, value, 1, 65535, con_gateway_port );
		}
		else if( key == "rta_gateway_port" )
		{
			ReadNumber( key, value, 1, 65535, rta_gateway_port );
		}
		else if( key == "client_timeout" )
		{
			ReadNumber( key, value, 1, 86400, client_timeout );
		}
		else if( key == "report_public_ip_for_lan_consoles" )
		{
			report_public_ip_for_lan_consoles = ReadSwitch( value );
		}
		else if( key == "max_connections" )
		{
			ReadNumber( key, value, 8, 100000, max_connections );
		}
		else if( key == "max_connections_per_address" )
		{
			ReadNumber( key, value, 1, 100000, max_connections_per_address );
		}
		else if( key == "first_message_timeout" )
		{
			ReadNumber( key, value, 5, 86400, first_message_timeout );
		}
		else if( key == "requests_per_second" )
		{
			ReadNumber( key, value, 1, 1000000, requests_per_second );
		}
		else if( key == "request_burst" )
		{
			ReadNumber( key, value, 100, 100000000, request_burst );
		}
		else if( key == "limits_for_local_addresses" )
		{
			limits_for_local_addresses = ReadSwitch( value );
		}
		else if( key == "max_database_mb" )
		{
			ReadNumber( key, value, 16, 1000000, max_database_mb );
		}
	}

	// If public_ip is not explicitly set, fall back to service_ip.
	// Note: service_ip can be 0.0.0.0 (bind-all), which is not routable by clients.
	// Set public_ip in config.ini to the server's external/LAN IP when service_ip is 0.0.0.0.
	if( public_ip.empty() )
	{
		public_ip = service_ip;
	}

	// A game with nothing to say sends an empty frame once it has sent nothing else for
	// more than ten of its own seconds, which it counts in lobby-task calls. Seen with
	// the real game: about every 760 frames, some 13 seconds at full speed and more
	// when the game runs slowly. Anything below this minimum would drop healthy
	// connections. The same time is how long a lost session can be picked up again.
	if( client_timeout < 15 )
	{
		Log::Warn( "client_timeout of {} seconds is too short, using 15.", client_timeout );
		client_timeout = 15;
	}

	// select() watches descriptors below FD_SETSIZE only, and the server needs some
	// for its listeners, its database and its log files.
	if( static_cast< int64_t >( max_connections ) > static_cast< int64_t >( MaxSelectableSockets() ) )
	{
		Log::Warn( "max_connections of {} is more than this build can watch, using {}.", max_connections, MaxSelectableSockets() );
		max_connections = static_cast< int32_t >( MaxSelectableSockets() );
	}

	// The game reads peer addresses as four numbers with dots, so a host name cannot
	// stand in for one, and a private address would be no better than the observed one.
	if( report_public_ip_for_lan_consoles && !Util::IsPublicIPv4( public_ip ) )
	{
		Log::Error( "report_public_ip_for_lan_consoles needs public_ip to be a public IPv4 address "
					"in dotted form, not [{}]. The setting is ignored.", public_ip );
		report_public_ip_for_lan_consoles = false;
	}

	return true;
}

std::string Config::ReportedAddress( const std::string &observedIp, const std::string &recipientIp )
{
	if( !report_public_ip_for_lan_consoles )
	{
		return observedIp;
	}

	// Only a console on the server's own network is observed at a private address.
	if( !Util::IsPrivateOrLoopbackIPv4( observedIp ) )
	{
		return observedIp;
	}

	// A player on that same network reaches the console directly at the observed
	// address and may not be able to reach it through the router at all.
	if( Util::IsPrivateOrLoopbackIPv4( recipientIp ) )
	{
		return observedIp;
	}

	return public_ip;
}