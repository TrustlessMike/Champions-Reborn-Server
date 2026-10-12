#pragma once

#include <cstdint>
#include <string>

#include "Platform.hpp"

namespace Util
{
	int32_t round_up( int32_t numToRound, int32_t multiple );
	int32_t round_down( int32_t numToRound, int32_t multiple );

	uint16_t ByteSwap( uint16_t val );
	uint32_t ByteSwap( uint32_t val );

	template <typename T>
	bool IsInRange( T value, T min, T max )
	{
		return ( value >= min && value <= max );
	}

	std::string IPFromAddr( const sockaddr_in &addr );

	// Classification of dotted IPv4 text. Anything that is not four numbers from
	// 0 to 255 separated by dots is neither private nor public.
	bool IsPrivateOrLoopbackIPv4( const std::string &ip );
	bool IsPublicIPv4( const std::string &ip );

	// The address as one number in host byte order (what ntohl gives for sin_addr).
	// False, and `out` untouched, for anything that is not four numbers with dots.
	bool IPv4ToNumber( const std::string &ip, uint32_t &out );

	// Wide strings in this codebase always hold UTF-16 code units, one per wchar_t,
	// whatever sizeof( wchar_t ) is on the host. That is what the game puts on the wire.
	std::string WideToUTF8( const std::wstring &wstr );
	std::wstring UTF8ToWide( const std::string &str );
}