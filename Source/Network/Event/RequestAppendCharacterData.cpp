#include "Network/Event/RequestAppendCharacterData.hpp"

#include "Game/CharacterSaveManager.hpp"
#include "Game/RealmUserManager.hpp"
#include "Game/RealmUser.hpp"
#include "Database/Database.hpp"
#include "logging.hpp"

void RequestAppendCharacterData::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	auto sessionId = stream->read_encrypted_utf16();

	auto length = stream->read_u32();
	m_data = stream->read_bytes( length );
	m_endOfData = stream->read_u32();
}

sptr_generic_response RequestAppendCharacterData::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto user = UserManager::Get().FindUserBySocket( socket );
	if( user == nullptr )
	{
		return std::make_shared< ResultAppendCharacterData >( this, FATAL_ERROR );
	}

	auto &saveManager = CharacterSaveManager::Get();

	auto task = saveManager.FindSaveTask( user->m_sessionId );
	if( task == nullptr )
	{
		// The request that should have started this upload was refused and has
		// been answered. The game goes on sending the pieces regardless.
		return nullptr;
	}

	const bool stored = saveManager.AppendSaveData( user->m_sessionId, m_data, m_endOfData != 0 );

	if( stored && !m_endOfData )
	{
		// The game matches replies to requests by tracking number alone, and every
		// piece carries the number of the 0x58 or 0x5E that started the upload. A
		// reply to this piece would be taken for the reply to that request.
		return nullptr;
	}

	if( !stored )
	{
		saveManager.m_tasks.erase( user->m_sessionId );
	}

	// The upload is over, one way or the other. Answer the request that started it.
	m_packetId = task->m_requestPacketId;
	m_trackId = task->m_requestTrackId;

	const bool isNewCharacter = ( task->m_saveType == CharacterSaveType::NEW_CHARACTER );

	return std::make_shared< ResultAppendCharacterData >( this, stored ? SUCCESS : FATAL_ERROR,
		isNewCharacter, stored ? task->m_characterId : 0 );
}

ResultAppendCharacterData::ResultAppendCharacterData( GenericRequest *request, int32_t reply, bool hasCharacterId, uint32_t characterId ) : GenericResponse( *request )
{
	m_reply = reply;
	m_hasCharacterId = hasCharacterId;
	m_characterId = characterId;
}

void ResultAppendCharacterData::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );

	// Reply to 0x58 (new character): the id the character was stored under.
	// Reply to 0x5E (save) has nothing after the result.
	if( m_hasCharacterId )
	{
		out.write_u32( m_characterId );
	}
}
