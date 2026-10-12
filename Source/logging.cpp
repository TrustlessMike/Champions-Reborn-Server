#include "logging.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <ctime>
#include <filesystem>

static struct tm LocalTime()
{
	time_t t; time( &t );
	struct tm date_tm;
#ifdef _WIN32
	localtime_s( &date_tm, &t );
#else
	localtime_r( &t, &date_tm );
#endif
	return date_tm;
}

// Takes the Windows console attribute values the rest of this file uses.
void Log::SetConsoleColor( int color )
{
#ifdef _WIN32
	SetConsoleTextAttribute( GetStdHandle( STD_OUTPUT_HANDLE ), static_cast< WORD >( color ) );
#else
	static const bool isTerminal = isatty( fileno( stdout ) );
	if( !isTerminal ) return;

	switch( color )
	{
		case 10: printf( "\033[92m" ); break;	// green
		case 11: printf( "\033[96m" ); break;	// cyan
		case 12: printf( "\033[91m" ); break;	// red
		case 13: printf( "\033[95m" ); break;	// magenta
		default: printf( "\033[0m" ); break;
	}
#endif
}

static const char *LOG_PATH[] = {
	"./generic",
	"./debug",
	"./error",
	"./warning",
};

Log::Log()
{
	current_open_hour = 0;
}

Log::~Log()
{
}

const char *Log::GetTimeStamp()
{
	static char timestamp[ 64 ] = "";

	struct tm date_tm = LocalTime();

	snprintf( timestamp, sizeof( timestamp ),
				 "[%4d-%02d-%02d %02d:%02d:%02d]: ",
				 ( date_tm.tm_year + 1900 ),
				 ( date_tm.tm_mon + 1 ),
				 date_tm.tm_mday,
				 date_tm.tm_hour,
				 date_tm.tm_min,
				 date_tm.tm_sec );

	return timestamp;
}

void Log::CheckFileStatus( LOG_TYPE type )
{
	struct tm date_tm = LocalTime();

	if( current_open_hour != date_tm.tm_hour )
	{
		current_open_hour = date_tm.tm_hour;
		if( file_stream[ type ].is_open() )
		{
			file_stream[ type ].close();
		}
	}

	if( !file_stream[ type ].is_open() )
	{
		char szFileName[ 256 ] = "";

		snprintf( szFileName, sizeof( szFileName ), "./log/%s/log_%04d.%02d.%02d_%02d.log",
					 LOG_PATH[ type ],
					 ( date_tm.tm_year + 1900 ),
					 ( date_tm.tm_mon + 1 ),
					 date_tm.tm_mday,
					 date_tm.tm_hour );

		std::error_code ec;
		std::filesystem::create_directories( std::filesystem::path( szFileName ).parent_path(), ec );

		file_stream[ type ].open( szFileName, std::fstream::out | std::fstream::app );
	}
}

// What clients send ends up in log lines: names, room names, chat handles. A line
// break in one would start a line of the sender's own making, an escape sequence would
// reach the terminal of whoever reads the log, and a long one would fill the disk
// faster. So: printable characters only, and no line longer than this.
static std::string Presentable( const std::string &message )
{
	constexpr size_t LONGEST = 600;

	std::string out;
	out.reserve( message.size() < LONGEST ? message.size() : LONGEST );

	for( const char ch : message )
	{
		if( out.size() >= LONGEST )
		{
			out += " ...";
			break;
		}

		const unsigned char byte = static_cast< unsigned char >( ch );
		out.push_back( ( byte < 0x20 || byte == 0x7F ) ? '?' : ch );
	}

	return out;
}

void Log::WriteToLog( LOG_TYPE type, const std::string &rawMessage )
{
	std::lock_guard lock( log_lock );

	// Lines per second, for each kind of line: a client can make the server write
	// one for every message it sends, and nothing else bounds the size of the log.
	// What is left out is counted and said once there is room again.
	{
		const auto now = std::chrono::steady_clock::now();
		auto &budget = line_budget[ type ];

		budget.Refill( now, LINES_PER_SECOND, LINES_AT_ONCE );

		if( !budget.Has( 1.0 ) )
		{
			suppressed_lines[ type ]++;
			return;
		}

		budget.Take( 1.0 );
	}

	std::string message = Presentable( rawMessage );

	if( suppressed_lines[ type ] > 0 )
	{
		message = "(" + std::to_string( suppressed_lines[ type ] ) + " lines of this kind were left out) " + message;
		suppressed_lines[ type ] = 0;
	}

	SetConsoleColor( type + 10 );
	switch( type )
	{
		case log_generic: printf( "[INFO]: " ); break;
		case log_debug:   printf( "[DEBUG]: " ); break;
		case log_warn:    printf( "[WARN]: " ); break;
		case log_error:   printf( "[ERROR]: " ); break;
		default: break;
	}

	SetConsoleColor( 15 );
	printf( "%s\n", message.c_str() );
	fflush( stdout );

	CheckFileStatus( type );

	file_stream[ type ] << GetTimeStamp() << message << '\n';
	file_stream[ type ].flush();
}

void Log::Packet( std::vector<uint8_t> p, size_t size, bool send )
{
#ifndef _DEBUG
	return;
#endif

	std::lock_guard<std::mutex> lock( log_lock );

	SetConsoleColor( 15 );

	uint16_t i = 0;
	uint8_t line[ 16 ] = {};
	uint8_t r = 0;

	SetConsoleColor( send ? 11 : 10 );
	printf( "(%s)(00|01|02|03|04|05|06|07|08|09|0A|0B|0C|0D|0E|0F)\n", send ? "SEND" : "RECV" );
	SetConsoleColor( 15 );

	while( i < size )
	{
		if( i % 16 == 0 )
		{
			if( i > 0 )
			{
				// Print ASCII characters for the previous line
				printf( " " );
				for( uint8_t j = 0; j < r; ++j )
				{
					char c = line[ j ];
					printf( "%c", ( c >= 0x20 && c <= 0x7E ) ? c : '.' );
				}
				printf( "\n" );
			}
			printf( "(%04X) ", i );
			r = 0;
		}

		line[ r++ ] = p[ i ];

		// Highlight packet type or flags
		if( i == 0 || i == 1 )
		{
			SetConsoleColor( send ? 11 : 10 );
			printf( "%02X ", p[ i ] );
			SetConsoleColor( 15 );
		}
		else
		{
			printf( "%02X ", p[ i ] );
		}

		++i;
	}

	// Padding for incomplete final line
	if( r > 0 )
	{
		for( uint8_t j = r; j < 16; ++j )
			printf( "   " ); // Pad to align character section

		printf( " " );
		for( uint8_t j = 0; j < r; ++j )
		{
			char c = line[ j ];
			printf( "%c", ( c >= 0x20 && c <= 0x7E ) ? c : '.' );
		}
	}

	printf( "\n\n" );
	fflush( stdout );
}

void Log::ToFile( std::string prefix, std::vector<uint8_t> p, size_t size )
{
	static char timestamp[ 64 ] = "";

	struct tm date_tm = LocalTime();

	snprintf( timestamp, sizeof( timestamp ),
				 "%02d-%02d-%02d.packet.bin",
				 date_tm.tm_hour,
				 date_tm.tm_min,
				 date_tm.tm_sec );

	std::string filename = prefix + '_' + timestamp;

	std::fstream file;
	file.open( filename, std::ios::out | std::ios::binary );

	if( !file.is_open() )
	{
		Error( "Failed to open packet log file: %s", filename.c_str() );
		return;
	}

	file.write( reinterpret_cast< const char * >( p.data() ), size );
	file.close();
}
