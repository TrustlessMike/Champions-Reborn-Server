#include <algorithm>
#include <cmath>
#include <string>
#include <cstdint>

#include "Common/Utility.hpp"

// Both work in 64 bits: with abs() and int32_t arithmetic the smallest and the largest
// values overflowed, and these numbers can come out of a message.
int32_t Util::round_up( int32_t numToRound, int32_t multiple )
{
	if( multiple <= 0 )
		return numToRound;

	const int64_t value = numToRound;
	const int64_t magnitude = value < 0 ? -value : value;
	const int64_t remainder = magnitude % multiple;

	if( remainder == 0 )
		return numToRound;

	const int64_t rounded = value < 0 ? -( magnitude - remainder ) : value + multiple - remainder;

	// Out of range only for the top few values; there is no multiple to round up to.
	return rounded > INT32_MAX ? numToRound : static_cast< int32_t >( rounded );
}

int32_t Util::round_down( int32_t numToRound, int32_t multiple )
{
	if( multiple <= 0 )
		return numToRound;

	const int64_t value = numToRound;
	const int64_t magnitude = value < 0 ? -value : value;
	const int64_t remainder = magnitude % multiple;

	if( remainder == 0 )
		return numToRound;

	const int64_t rounded = value < 0 ? -( magnitude + remainder ) : value - remainder;

	return rounded < INT32_MIN ? numToRound : static_cast< int32_t >( rounded );
}

uint16_t Util::ByteSwap( uint16_t val )
{
	return ( val << 8 ) | ( val >> 8 );
}

uint32_t Util::ByteSwap( uint32_t val )
{
	return ( ( val << 24 ) & 0xFF000000 ) |
		( ( val << 8 ) & 0x00FF0000 ) |
		( ( val >> 8 ) & 0x0000FF00 ) |
		( ( val >> 24 ) & 0x000000FF );
}

std::string Util::IPFromAddr( const sockaddr_in &addr )
{
	char buffer[ INET_ADDRSTRLEN ] = {};
	const char *result = inet_ntop( AF_INET, &addr.sin_addr, buffer, sizeof( buffer ) );

	if( result )
		return std::string( buffer );
	else
		return {};
}

// Parses "a.b.c.d" without touching the socket library, so it behaves the same everywhere.
static bool ParseIPv4( const std::string &ip, uint8_t octets[ 4 ] )
{
	size_t pos = 0;

	for( int i = 0; i < 4; ++i )
	{
		size_t digits = 0;
		uint32_t value = 0;

		while( pos < ip.size() && ip[ pos ] >= '0' && ip[ pos ] <= '9' && digits < 3 )
		{
			value = value * 10 + static_cast< uint32_t >( ip[ pos ] - '0' );
			++pos;
			++digits;
		}

		if( digits == 0 || value > 255 )
		{
			return false;
		}

		octets[ i ] = static_cast< uint8_t >( value );

		if( i < 3 )
		{
			if( pos >= ip.size() || ip[ pos ] != '.' )
			{
				return false;
			}
			++pos;
		}
	}

	return pos == ip.size();
}

bool Util::IsPrivateOrLoopbackIPv4( const std::string &ip )
{
	uint8_t o[ 4 ];
	if( !ParseIPv4( ip, o ) )
	{
		return false;
	}

	return o[ 0 ] == 10									// 10.0.0.0/8
		|| ( o[ 0 ] == 172 && o[ 1 ] >= 16 && o[ 1 ] <= 31 )	// 172.16.0.0/12
		|| ( o[ 0 ] == 192 && o[ 1 ] == 168 )			// 192.168.0.0/16
		|| o[ 0 ] == 127;								// loopback
}

bool Util::IsPublicIPv4( const std::string &ip )
{
	uint8_t o[ 4 ];
	if( !ParseIPv4( ip, o ) )
	{
		return false;
	}

	if( IsPrivateOrLoopbackIPv4( ip ) )
	{
		return false;
	}

	return o[ 0 ] != 0									// "this network", including 0.0.0.0
		&& !( o[ 0 ] == 169 && o[ 1 ] == 254 )			// link-local
		&& o[ 0 ] < 224;								// multicast and reserved
}

static void AppendUTF8( std::string &out, uint32_t cp )
{
	if( cp < 0x80 )
	{
		out.push_back( static_cast< char >( cp ) );
	}
	else if( cp < 0x800 )
	{
		out.push_back( static_cast< char >( 0xC0 | ( cp >> 6 ) ) );
		out.push_back( static_cast< char >( 0x80 | ( cp & 0x3F ) ) );
	}
	else if( cp < 0x10000 )
	{
		out.push_back( static_cast< char >( 0xE0 | ( cp >> 12 ) ) );
		out.push_back( static_cast< char >( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
		out.push_back( static_cast< char >( 0x80 | ( cp & 0x3F ) ) );
	}
	else
	{
		out.push_back( static_cast< char >( 0xF0 | ( cp >> 18 ) ) );
		out.push_back( static_cast< char >( 0x80 | ( ( cp >> 12 ) & 0x3F ) ) );
		out.push_back( static_cast< char >( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
		out.push_back( static_cast< char >( 0x80 | ( cp & 0x3F ) ) );
	}
}

std::string Util::WideToUTF8( const std::wstring &wstr )
{
	std::string result;
	result.reserve( wstr.size() );

	for( size_t i = 0; i < wstr.size(); ++i )
	{
		uint32_t cp = static_cast< uint16_t >( wstr[ i ] );

		// Combine a surrogate pair; a lone surrogate becomes U+FFFD.
		if( cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wstr.size() )
		{
			const uint32_t low = static_cast< uint16_t >( wstr[ i + 1 ] );
			if( low >= 0xDC00 && low <= 0xDFFF )
			{
				cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( low - 0xDC00 );
				++i;
			}
		}

		if( cp >= 0xD800 && cp <= 0xDFFF )
		{
			cp = 0xFFFD;
		}

		AppendUTF8( result, cp );
	}

	return result;
}

std::wstring Util::UTF8ToWide( const std::string &str )
{
	std::wstring result;
	result.reserve( str.size() );

	for( size_t i = 0; i < str.size(); )
	{
		const uint8_t lead = static_cast< uint8_t >( str[ i ] );

		uint32_t cp = 0xFFFD;
		size_t length = 1;

		if( lead < 0x80 )
		{
			cp = lead;
		}
		else if( ( lead & 0xE0 ) == 0xC0 )
		{
			cp = lead & 0x1F;
			length = 2;
		}
		else if( ( lead & 0xF0 ) == 0xE0 )
		{
			cp = lead & 0x0F;
			length = 3;
		}
		else if( ( lead & 0xF8 ) == 0xF0 )
		{
			cp = lead & 0x07;
			length = 4;
		}

		if( i + length > str.size() )
		{
			cp = 0xFFFD;
			length = 1;
		}

		for( size_t j = 1; j < length; ++j )
		{
			const uint8_t next = static_cast< uint8_t >( str[ i + j ] );
			if( ( next & 0xC0 ) != 0x80 )
			{
				cp = 0xFFFD;
				length = j;
				break;
			}
			cp = ( cp << 6 ) | ( next & 0x3F );
		}

		i += length;

		if( cp >= 0x10000 )
		{
			cp -= 0x10000;
			result.push_back( static_cast< wchar_t >( 0xD800 + ( cp >> 10 ) ) );
			result.push_back( static_cast< wchar_t >( 0xDC00 + ( cp & 0x3FF ) ) );
		}
		else
		{
			result.push_back( static_cast< wchar_t >( cp ) );
		}
	}

	return result;
}
