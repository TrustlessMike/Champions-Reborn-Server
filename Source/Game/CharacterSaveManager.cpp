/*
* CharacterSaveManager is responsible for managing character saves while online.
* Because Return To Arms can have any user in the party initiate a save for any
* other user in the party, we need to keep track of these tasks.
*
* Tasks keep track of the user that initiated the save (the owner),
* and the target user whose character is being saved.
*/

#include "Game/CharacterSaveManager.hpp"

#include "Game/RealmUser.hpp"
#include "Game/RealmUserManager.hpp"
#include "Database/Database.hpp"
#include "Lobby Server/LobbyServer.hpp"
#include "logging.hpp"

CharacterSaveManager::CharacterSaveManager()
{
	m_tasks.clear();
}

CharacterSaveManager::~CharacterSaveManager()
{
	m_tasks.clear();
}

bool CharacterSaveManager::BeginSaveTask(
	const sptr_user user,
	const uint32_t characterId,
	const CharacterSlotData &metaData,
	const CharacterSaveType saveType )
{
	return BeginSaveTask( user, user, characterId, metaData, saveType );
}

bool CharacterSaveManager::BeginSaveTask(
	const sptr_user m_owner,
	const sptr_user m_target,
	const uint32_t characterId,
	const CharacterSlotData &metaData,
	const CharacterSaveType saveType,
	const int16_t requestPacketId,
	const uint32_t requestTrackId )
{
	if( !m_owner || !m_target )
	{
		Log::Error( "CreateSaveTask called with empty session IDs!" );
		return false;
	}

	const auto &sessionId = m_owner->m_sessionId;

	// Only a logged-in session uploads, and only to an account.
	if( sessionId.empty() || m_target->m_accountId <= 0 )
	{
		Log::Error( "CreateSaveTask called without a session or an account!" );
		return false;
	}

	if( m_tasks.find( sessionId ) != m_tasks.end() )
	{
		m_tasks.erase( sessionId );
	}

	auto task = std::make_shared< CharacterSaveTask >( saveType, characterId );

	task->m_ownerUser = m_owner;
	task->m_targetUser = m_target;
	task->m_targetAccountId = m_target->m_accountId;
	task->m_meta = metaData;
	task->m_requestPacketId = requestPacketId;
	task->m_requestTrackId = requestTrackId;
	m_tasks[ sessionId ] = task;

	return true;
}

bool CharacterSaveManager::AppendSaveData( const std::wstring &sessionId, const std::vector<uint8_t> &data, bool endOfData )
{
	try
	{
		auto it = m_tasks.find( sessionId );
		if( it == m_tasks.end() )
		{
			Log::Error( "AddDataToSaveTask: No task found for session ID [{}].", sessionId );
			return false;
		}

		// Keeps the task alive after CommitSaveTask has taken it out of the map.
		auto task = it->second;

		// The game cuts the data into pieces of 800 bytes. When everything fitted into
		// the first message it still sends a last piece, flagged and empty.
		if( !( data.empty() && endOfData ) && !task->AppendData( data ) )
		{
			Log::Error( "AddDataToSaveTask: Append failed for session ID [{}].", sessionId );
			return false;
		}

		if( !endOfData )
		{
			return true;
		}

		if( !task->Validate() )
		{
			Log::Error( "AddDataToSaveTask: Validation failed for session ID [{}].", sessionId );
			return false;
		}

		if( !CommitSaveTask( sessionId ) )
		{
			Log::Error( "AddDataToSaveTask: Commit failed for session ID [{}].", sessionId );
			return false;
		}

		Log::Debug( "AddDataToSaveTask: Final chunk committed for session ID [{}].", sessionId );
		return true;
	}
	catch( const std::exception &e )
	{
		Log::Error( "AddDataToSaveTask: Exception for session ID [{}]: {}", sessionId, std::string( e.what() ) );
	}

	return false;
}

bool CharacterSaveManager::CommitSaveTask( const std::wstring &sessionId )
{
	try
	{
		auto node = m_tasks.extract( sessionId );
		if( !node )
		{
			Log::Error( "CommitSaveTask: Task for session ID [{}] not found.", sessionId );
			return false;
		}

		const auto &saveTask = node.mapped();

		if( !saveTask->Validate() )
		{
			return false;
		}

		auto &targetUser = saveTask->m_targetUser;

		if( !targetUser )
		{
			Log::Error( "CommitSaveTask: Target user not found." );
			return false;
		}

		// The account is the one the upload was begun for. The user may have gone
		// since (its connection lost while another console saved its character), or
		// logged in as somebody else.
		const int64_t accountId = saveTask->m_targetAccountId;

		// The database write is the expensive part of an upload.
		LobbyServer::Get().AddRequestCost( 30 );

		if( saveTask->m_saveType == CharacterSaveType::NEW_CHARACTER )
		{
			// The game itself stops offering "new character" at this number, and its
			// character list overruns at 50; nothing else may get an account there.
			if( Database::Get().LoadCharacterSlots( accountId ).size() >= MAX_NUMBER_OF_CHARACTERS )
			{
				Log::Error( "CommitSaveTask: Account ID {} already has {} characters", accountId, MAX_NUMBER_OF_CHARACTERS );
				return false;
			}

			auto result = Database::Get().CreateNewCharacter( accountId, saveTask->m_meta, saveTask->m_data );
			if( !result )
			{
				Log::Error( "CommitSaveTask: Failed to create new character for account ID: {}", accountId );
				return false;
			}

			Log::Debug( "CommitSaveTask: New character created with ID {} for account ID: {}", result, accountId );

			// The user may have become somebody else's since (EndSession sets -1).
			if( targetUser->m_isLoggedIn )
			{
				targetUser->m_characterId = result;
			}

			saveTask->m_characterId = result;
		}
		else
		{
			if( 0 == saveTask->m_characterId )
			{
				Log::Error( "CommitSaveTask: Invalid character ID for save task." );
				return false;
			}

			auto result = Database::Get().SaveCharacter( accountId, static_cast< int32_t >( saveTask->m_characterId ), saveTask->m_meta, saveTask->m_data );
			if( !result )
			{
				Log::Error( "CommitSaveTask: Failed to save character for account ID: {}", accountId );
				return false;
			}

			Log::Debug( "CommitSaveTask: Character saved for account ID: {}", accountId );
		}
	}
	catch( const std::exception &e )
	{
		Log::Error( "CommitSaveTask: Exception occurred while committing task for session ID [{}]: {}", sessionId, std::string( e.what() ) );
		return false;
	}

	Log::Debug( "CommitSaveTask: Task for session ID [{}] committed successfully.", sessionId );
	return true;
}

void CharacterSaveManager::RemoveSaveTask( const std::wstring &sessionId )
{
	auto it = m_tasks.find( sessionId );
	if( it != m_tasks.end() )
	{
		m_tasks.erase( it );
		Log::Debug( "RemoveSaveTask: Task for session ID [{}] removed.", sessionId );
	}
	else
	{
		Log::Error( "RemoveSaveTask: Task for session ID [{}] not found.", sessionId );
	}
}

sptr_character_save_task CharacterSaveManager::FindSaveTask( const std::wstring &sessionId )
{
	auto it = m_tasks.find( sessionId );
	if( it != m_tasks.end() )
	{
		return it->second;
	}
	else
	{
		Log::Error( "FindSaveTask: Task for session ID [{}] not found.", sessionId );
		return nullptr;
	}
}
