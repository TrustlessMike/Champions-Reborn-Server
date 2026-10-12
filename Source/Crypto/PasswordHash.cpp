#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <stdexcept>

#include "Crypto/PasswordHash.hpp"

std::string HexDump( const std::vector<uint8_t> &bytes )
{
    std::ostringstream oss;
    for( auto b : bytes )
        oss << std::hex << std::setfill( '0' ) << std::setw( 2 ) << ( int )b;
    return oss.str();
}

std::string HashPassword( const std::string &password, uint32_t iterations, size_t saltLen )
{
    // Every byte from the system's random source. (The salt used to come from a
    // generator seeded with 32 bits, so there were only 2^32 different salts.)
    std::vector<uint8_t> salt( saltLen );
    std::random_device rd;

    for( size_t i = 0; i < salt.size(); )
    {
        uint32_t word = static_cast< uint32_t >( rd() );

        for( int k = 0; k < 4 && i < salt.size(); ++k, ++i )
        {
            salt[ i ] = static_cast< uint8_t >( word & 0xFF );
            word >>= 8;
        }
    }

    auto derived = pbkdf2_hmac_sha256( password, salt, iterations, 32 );

    return "pbkdf2$" + std::to_string( iterations ) + "$" +
        Base64Encode( salt ) + "$" +
        Base64Encode( derived );
}

static std::vector<std::string> SplitHash( const std::string &storedHash )
{
    auto parts = std::vector<std::string>();
    std::stringstream ss( storedHash );
    std::string token;
    while( std::getline( ss, token, '$' ) )
        parts.push_back( token );

    if( parts.size() != 4 || parts[ 0 ] != "pbkdf2" )
        throw std::runtime_error( "Invalid hash format" );

    return parts;
}

bool VerifyPassword( const std::string &password, const std::string &storedHash )
{
    const auto parts = SplitHash( storedHash );

    const unsigned long iterations = std::stoul( parts[ 1 ] );
    std::vector<uint8_t> salt = Base64Decode( parts[ 2 ] );
    std::vector<uint8_t> expected = Base64Decode( parts[ 3 ] );

    if( iterations == 0 || iterations > PASSWORD_MAX_ROUNDS || expected.empty() || expected.size() > 64 )
        throw std::runtime_error( "Invalid hash parameters" );

    auto derived = pbkdf2_hmac_sha256( password, salt, static_cast< uint32_t >( iterations ), expected.size() );

    // Every byte is looked at whatever the outcome, so the time taken does not say
    // how much of a guess was right.
    uint8_t difference = 0;
    for( size_t i = 0; i < expected.size(); ++i )
        difference |= static_cast< uint8_t >( derived[ i ] ^ expected[ i ] );

    return difference == 0;
}

bool PasswordNeedsRehash( const std::string &storedHash )
{
    try
    {
        return std::stoul( SplitHash( storedHash )[ 1 ] ) < PASSWORD_ROUNDS;
    }
    catch( const std::exception & )
    {
        return false;
    }
}
