#pragma once

#include <vector>
#include <string>
#include <memory>
#include <iterator>
#include <optional>
#include <cstring>
#include <stdexcept>

#include "Utility.hpp"
#include "../Crypto/RealmCrypt.hpp"

class ByteBuffer {
public:
	ByteBuffer( const std::vector< uint8_t > &data );
	ByteBuffer( const std::string &data );
	ByteBuffer( const uint8_t *data, uint32_t length );
	ByteBuffer( uint32_t length );
	ByteBuffer();

	~ByteBuffer();

	void resize( uint32_t size );
	void shrink_to_fit();

	template < typename T >
	void write( T value );

	template < typename T >
	T read();

	void forward( uint32_t length ) {
		m_position += length;
		if ( m_position > m_buffer.size() ) {
			m_position = m_buffer.size();
		}
	}

	void write_u8( uint8_t value );
	void write_u16( uint16_t value );
	void write_u32( uint32_t value );
	void write_i8( int8_t value );
	void write_i16( int16_t value );
	void write_i32( int32_t value );
	void write_f32( float_t value );

	void write_utf8( const std::string &str, std::optional<uint32_t> length = std::nullopt );
	void write_utf16( const std::wstring &str, std::optional<uint32_t> length = std::nullopt );
	void write_sz_utf8( const std::string &str, std::optional<uint32_t> length = std::nullopt );
	void write_sz_utf16( const std::wstring &str, std::optional<uint32_t> length = std::nullopt );
	void write_encrypted_utf8( const std::string &str );
	void write_encrypted_utf16( const std::wstring &str );

	uint8_t read_u8();
	uint16_t read_u16();
	uint32_t read_u32();
	int8_t read_i8();
	int16_t read_i16();
	int32_t read_i32();
	float_t read_f32();

	std::string read_utf8( std::optional<uint32_t> length = std::nullopt );
	std::wstring read_utf16( std::optional<uint32_t> length = std::nullopt );
	std::string read_sz_utf8();
	std::wstring read_sz_utf16();
	std::string read_encrypted_utf8( bool hasBlockLength = true );
	std::wstring read_encrypted_utf16( bool hasBlockLength = true );

	void write_bytes( const std::vector< uint8_t > &value );
	void write_bytes( const uint8_t *value, uint32_t length );
	void write_encrypted_bytes( const std::vector< uint8_t > &value );

	std::vector< uint8_t > read_bytes( uint32_t length );
	std::vector< uint8_t > read_encrypted_bytes( uint32_t length );

	std::vector< uint8_t > get_buffer() const;

	size_t get_length() const;
	size_t get_position() const;
	void set_position( size_t where );

	// Bytes left between the read position and the end of the buffer.
	size_t remaining() const
	{
		return m_position < m_buffer.size() ? m_buffer.size() - m_position : 0;
	}

	// Every read goes through this. A message that is shorter than its own length
	// fields claim must not be read past its end; the caller catches the exception.
	void require( size_t length, const char *what ) const
	{
		if( length > remaining() )
		{
			throw std::out_of_range( std::string( what ) + ": Attempt to read past end of buffer" );
		}
	}

	std::vector< uint8_t > m_buffer;
	size_t m_position;
};

// Defined here rather than in ByteStream.cpp because other translation units call
// these with their own types, and only MSVC exports the implicit instantiations.
template < typename T >
void ByteBuffer::write( T value )
{
	write_bytes( ( uint8_t * )&value, sizeof( T ) );
}

template < typename T >
T ByteBuffer::read()
{
	require( sizeof( T ), "read" );

	T value;
	std::memcpy( &value, &m_buffer[ m_position ], sizeof( T ) );
	m_position += sizeof( T );

	return value;
}

typedef std::shared_ptr< ByteBuffer > sptr_byte_stream;