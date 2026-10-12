#include "Network/Event/RequestSaveCharacter_RTA.hpp"

#include "Database/Database.hpp"
#include "Game/GameSessionManager.hpp"
#include "Game/CharacterSaveManager.hpp"
#include "Game/RealmUserManager.hpp"
#include "Game/RealmUser.hpp"
#include "Game/RealmCharacter.hpp"
#include "logging.hpp"

#include "Common/RLEZ.hpp"

void RequestSaveCharacter_RTA::Deserialize( sptr_byte_stream stream )
{
	DeserializeHeader( stream );

	m_sessionId = stream->read_encrypted_utf16();
	m_memberSessionId = stream->read_encrypted_utf16();
	m_targetCharacterId = stream->read_u32();

	auto a = stream->read_u32();
	auto b = stream->read_u32();

	m_metaData.Deserialize( stream );
	auto characterDataSize = stream->read_u32();
	m_characterData = stream->read_bytes( characterDataSize );
}

sptr_generic_response RequestSaveCharacter_RTA::ProcessRequest( sptr_socket socket, sptr_byte_stream stream )
{
	Deserialize( stream );

	auto &userManager = UserManager::Get();

	auto user = userManager.FindLoggedInUser( socket );
	if( user == nullptr || user->m_accountId == -1 )
	{
		return std::make_shared< ResultSaveCharacter_RTA >( this, FATAL_ERROR );
	}

	sptr_user targetUser = user;

	// Who is asking is settled by the connection, not by the session id written at
	// the front of the request. Only the second id, whose character this is, is
	// looked up.
	if( m_memberSessionId != user->m_sessionId )
	{
		targetUser = userManager.FindUserBySessionId( m_memberSessionId );

		// Saving another player's character is how the game saves: one console of a
		// party is picked for each character. But only within a party. The two must
		// have opened or joined the same game; knowing a session id is not enough.
		if( targetUser != nullptr
			&& ( user->m_partyGameId < 0 || targetUser->m_partyGameId != user->m_partyGameId
				 || targetUser->m_gameType != user->m_gameType ) )
		{
			Log::Error( "[{}] tried to save the character of [{}], who is not in a game with it",
						user->m_username, targetUser->m_username );
			return std::make_shared< ResultSaveCharacter_RTA >( this, FATAL_ERROR );
		}
	}

	if( targetUser == nullptr || targetUser->m_accountId == -1 )
	{
		Log::Error( "Target user not found or invalid account ID for session: {}", m_memberSessionId );
		return std::make_shared< ResultSaveCharacter_RTA >( this, FATAL_ERROR );
	}

	auto characterId = targetUser->m_characterId;

	if( characterId == 0 )
	{
		Log::Error( "Target user has not created or loaded a character to save! [{}]", m_memberSessionId );
		return std::make_shared< ResultSaveCharacter_RTA >( this, FATAL_ERROR );
	}

	auto &saveManager = CharacterSaveManager::Get();
	if( !saveManager.BeginSaveTask( user, targetUser, characterId, m_metaData, CharacterSaveType::SAVE_CHARACTER, m_packetId, m_trackId )
		|| !saveManager.AppendSaveData( user->m_sessionId, m_characterData, false ) )
	{
		saveManager.m_tasks.erase( user->m_sessionId );
		return std::make_shared< ResultSaveCharacter_RTA >( this, FATAL_ERROR );
	}

	// No reply yet, see RequestCreateNewCharacter_RTA: the game keeps its "saving"
	// state until this request is answered, and that has to wait for the last piece.
	return nullptr;
}

ResultSaveCharacter_RTA::ResultSaveCharacter_RTA( GenericRequest *request, int32_t reply ) : GenericResponse( *request )
{
	m_reply = reply;
}

void ResultSaveCharacter_RTA::Serialize( ByteBuffer &out ) const
{
	out.write_u16( m_packetId );
	out.write_u32( m_trackId );
	out.write_u32( m_reply );
}
