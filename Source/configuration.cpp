#include "configuration.hpp"

#include "Common/Utility.hpp"
#include "logging.hpp"

bool Config::Load( std::string filename )
{
	service_ip = "0.0.0.0";
	public_ip = "";
	con_lobby_port = 40900;
	rta_lobby_port = 40910;
	discovery_port = 10101;
	client_timeout = 120;
	report_public_ip_for_lan_consoles = false;

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
			con_lobby_port = std::stoi( value );
		}
		else if (key == "rta_lobby_port")
		{
			rta_lobby_port = std::stoi(value);
		}
		else if( key == "discovery_port" )
		{
			discovery_port = std::stoi( value );
		}
		else if( key == "client_timeout" )
		{
			client_timeout = std::stoi( value );
		}
		else if( key == "report_public_ip_for_lan_consoles" )
		{
			report_public_ip_for_lan_consoles = ( value == "1" || value == "true" || value == "on" || value == "yes" );
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