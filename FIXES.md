# Fixes to the Champions Reborn server

Changes made on 2026-10-10 to the working tree in `server/upstream` (branch `macos-port`,
nothing committed), on top of the macOS/Linux port. They are meant to let a group of friends
play Return to Arms through this server.

How to read this file:

- "The game" is the Return to Arms executable SLUS_209.73. An address such as 0x3841dc is a
  function in it; that is where the correct behaviour was read. Nothing here was run against
  a real console or an emulator. (Sections 16 to 18 came a day later and start from what
  the game did against this server in an emulator; see there. Section 19 came after
  those three had themselves been run with the game in that emulator. The runs are
  `docs/issues/live-rig.md` sections 19 and 20 of the repository named in the README.)
- "Before the fix" results come from running the new test against a build of the tree as it
  was before these changes (rebuilt in a scratch directory from `git archive` plus the port's
  diff). That build failed 17 of the 33 steps and its process died in step 28.
- The test is `tools/lobby_e2e_test.py`. `python3 tools/lobby_e2e_test.py 127.0.0.1` runs
  16 steps, `--extended` 33. Both pass in full against the fixed server, also with
  `report_public_ip_for_lan_consoles=1`, and also against a build with the address and
  undefined-behaviour sanitizers, which reported nothing. (Since section 19 `--extended`
  runs 40: seven steps were added behind the others, and the first 33 kept their
  numbers.)
- D1 to D10 are the defect numbers of `docs/lobby-protocol.md` section 10.
- Sections 21 to 30 are of another kind: what a stranger could do to a server that is
  reachable from the internet, each with its severity and its reproducer. What is and
  is not protected after them is in `SECURITY.md`.

Build: `sh build.sh` in `server/upstream`. The Windows project files list the three new
sources, and nothing POSIX-only was added outside `Include/Platform.hpp`, but **the Windows
build was not compiled**.

## 1. Game list unreadable with two or more public games (D1)

- Wrong: array 4 of the 0x55 reply was written as a count followed by a single `u32`,
  whatever the count. The game reads one length-prefixed blob per game.
- Changed: `ResultMatchGame_RTA::Serialize`, Source/Network/Event/RequestMatchGame_RTA.cpp:
  one empty blob (a zero length) per game.
- Established by: 0x3841dc case 0x55 reads the fourth array with the blob-list reader
  0x3df2cc, which calls the blob reader 0x389f10 once per element.
- Test: step 24, "two public games open at once". Before the fix: unparseable.

## 2. Private games handed out the wrong addresses (D2)

- Wrong: the 0x57 reply put the host's observed address first and its LAN address second,
  both with the observed port. It also answered before the host's address was known, with an
  empty address.
- Changed: `RequestGetGame_RTA::ProcessRequest` and `ResultGetGame_RTA` (constructor,
  `Serialize`), Source/Network/Event/RequestGetGame_RTA.cpp and its header. Order is now LAN
  address, LAN port, observed address, observed port, taken from the same four fields of the
  game session that the public list uses. A game that is not open yet gives result 2.
- Established by: the call at 0x386b6c passes the third string and third number of the reply
  before the second ones; the handler 0x329880 stores those at client+0x148/+0x150 and the
  others at +0x14c/+0x152, and builds a peer record with +0x148/+0x150 in the external slot
  (record +0x90/+0x98) and +0x14c/+0x152 in the internal slot (+0x94/+0x9a). So: first pair
  internal, second pair external, as `docs/lobby-protocol.md` 9.4 says.
- Test: step 23, "private game found by name". It now also joins the game and checks that
  the game is not handed out before the host's discovery datagram.

## 3. Login reply did not carry the chat handle (D3)

- Wrong: the second string of the 0x16 reply was a fixed placeholder.
- Changed: `RequestLogin::ProcessLoginRTA`, `ResultLogin` (constructor, `Serialize`),
  Source/Network/Event/RequestLogin.cpp and its header. Return to Arms gets the account's
  chat handle; Champions of Norrath, which has none, still gets the placeholder.
- Established by: 0x32eba8 copies that string, narrowed to bytes and without a length limit,
  to client+0xca1. That is the second string (offset +0x21, 32 bytes) of the console's own
  peer record at +0xc80, the record it sends to the other players of a game. The same field
  is filled from what the user typed at account creation (0x330950).
- Test: step 16. Before the fix: garbage.

## 4. Strings that are not a multiple of 16 bytes were encrypted from a dead buffer (D4)

- Wrong: in `encryptSymmetric` and `decryptSymmetric` the padded copy was a vector local to
  an `if` block, used through a span after the block had ended. Every chat handle is such a
  string. The result buffer was also never freed, a leak on every encrypted field.
- Changed: both functions in Source/Crypto/RealmCrypt.cpp. The padded copy now lives as
  long as the function, and the result is freed after copying.
- Established by: reading the code; step 16 before the fix shows different garbage for the
  same input. The game side accepts ciphertext padded to the next multiple of 16: 0x38d14c
  decrypts ((length − 4) rounded down to 16) + 16 bytes of a zero-filled buffer and keeps
  the plain length given in front.
- Test: step 16.

## 5. Characters stored on the server (D5)

Four things were wrong.

**a. The reply to 0x58 had no character id, and it came too early.** The game reads a `u32`
after the result (0x3841dc case 0x58) and files it, per player, as the id of the character
(0x328090 writes it to the table at 0x4f85d0); the same table supplies the id in every later
save (0x3278f0 passes it to the 0x5E builder 0x380f08). The id cannot exist before the data
is stored. And the game does not expect a reply per piece at all:

- every 0x5D piece carries the tracking number of the 0x58 or 0x5E that started the upload
  (0x37ca08 uses the number kept by 0x380888 / 0x380f08);
- a reply is matched to a request by tracking number only, and parsed according to the
  request that is pending under that number, not according to the id in the reply
  (0x381f80);
- a request already pending is not replaced when another is sent under the same number
  (0x3dd088 returns the existing entry), so while the 0x58 is unanswered every piece counts
  as part of it;
- the reply parser has no handling for 0x5D (0x3841dc: `case 0x5d: return`), and neither has
  the error path (0x383858);
- while a request is unanswered the game's "busy" flag 0x506154 is set, and a new save is
  silently skipped (0x3278f0). Once it is clear, a save that starts while pieces of the
  previous one are still going out fails in the game itself (the builders refuse while
  the upload flag at +0x2c of the API object is set, and report result 45), which shows
  the save error message.

So one upload has one reply: to the request that started it, after the last piece.

**b. A character whose packed data fits into the create request was never stored.** The
game cuts the data into pieces of 800 bytes; when everything fits into the request it still
sends a last piece, flagged and empty (0x380888 sets the offset to the length; 0x37ca08
then sends the remaining 0 bytes with the flag). The server refused empty pieces before it
looked at the flag. A new character is mostly zeros and packs to a few hundred bytes.
Confirmed against the build before the fix: nothing stored, both messages answered 0.

**c. A failed store was reported as success** (validation or database), because nothing was
passed back from `AppendSaveData`.

**d. An unfinished upload kept its user object alive**, and with it the socket: the
connection was never closed.

- Changed:
  - Source/Network/Event/RequestCreateNewCharacter_RTA.cpp, `ProcessRequest`: no reply
    unless the request is refused at once; `ResultCreateNewCharacter_RTA::Serialize` writes
    the id. A user who is not logged in is refused.
  - Source/Network/Event/RequestSaveCharacter_RTA.cpp, `ProcessRequest`: the same; refused
    at once when the target has no current character.
  - Source/Network/Event/RequestAppendCharacterData.cpp, `ProcessRequest` and
    `ResultAppendCharacterData`: no reply to a piece that is not the last; after the last
    one, or when a piece is refused, the reply to the original request (its id and tracking
    number), with the new id for 0x58. A piece with no upload in progress is ignored.
  - Source/Game/CharacterSaveManager.cpp: `BeginSaveTask` keeps the request's id and
    tracking number; `AppendSaveData` returns success or failure and accepts an empty last
    piece; `CommitSaveTask` records the new id in the task and refuses a thirteenth
    character (the constant `MAX_NUMBER_OF_CHARACTERS` existed and was unused; the game
    stops offering "new character" at 12, 0x225478, and its list holds 50 entries, 0x222a8c).
  - Source/Game/RealmCharacterSaveTask.cpp, `AppendData`: an upload cannot grow beyond
    twice the character size plus four bytes.
  - Source/Game/RealmUserManager.cpp, `RemoveUser`: drops the user's unfinished upload.
  - Source/Network/Event/RequestGetCharacterData_RTA.cpp, `ProcessRequest`: a character
    with no data gets a failure reply instead of none.
- The replies to 0x5B (list), 0x5C (load), 0x5A (delete) and 0x5E (save) were compared
  with the game's parser and needed no change: 0x5B is `u32` count, then per character
  `u32` id, `u32` pair count, pairs of two wide strings (0x3df464, 0x38a4ac, 0x3e0dd4,
  0x38a520); 0x5C is a blob of at most 1024 bytes and a `u32` last-piece flag, one frame per
  piece, each matched by the request's tracking number (0x3841dc case 0x5c); 0x5A and 0x5E
  have nothing after the result.
- Test: step 17 creates (request plus empty last piece), lists, loads, saves in three
  messages and in exactly two, checks that nothing comes back before the last piece and
  exactly one frame after it, that the id in the create reply is the id in the list, that
  data which cannot be a character is refused with a failure result, and that a stray piece
  is ignored. Step 18 checks the `u32`. Before the fix: step 17 fails on the early reply.

## 6. Start-game reply was short (D6)

- Changed: `ResultStartGame::Serialize`, Source/Network/Event/RequestStartGame.cpp: a `u32`
  0 after the result.
- Established by: 0x3841dc case 0x23 reads one `u32` and reports result 45 without it. The
  callback 0x32f938 only sets the status text, so the value is not used.
- Test: step 18.

## 7. Game chat room: empty reply, and message 0x19 (D7)

- Wrong: the 0x09 reply had no body, so the game saw result 45 and never sent 0x19; and
  there was no handler for 0x19.
- 0x19 is: session, room name, text. The game sends it from the 0x09 reply handler
  (0x32ef50) with the fixed line "This chat-room is private for the members of this game."
  A player entering a room is shown the room's second string as a chat line if it is not
  empty (0x32f038, the 0x0C reply handler). So 0x19 sets that string. Its reply has nothing
  after the result (0x3841dc case 0x19, callback 0x32f680).
- Changed:
  - Source/Network/Event/RequestCreatePrivateRoom.cpp, `ProcessRequest` and
    `ResultCreatePrivateRoom`: the reply carries name, text, members, moderators as the
    0x0C reply does (0x3841dc case 9 reads the same four fields), result 1 with empty
    fields when the room cannot be created.
  - New Source/Network/Event/RequestSetRoomBanner.cpp and header, registered as 0x19 in
    Include/Network/Events.hpp: the owner of a room sets its text (cut at the 1000
    characters the game accepts).
  - Source/Network/Event/RequestCancelGame_RTA.cpp, `ProcessRequest`: leaving a game leaves
    its chat room even when the game is already gone. Before, a player who left after the
    host stayed in the room for good, and the next game of that name could not have one.
- Test: steps 18 and 25 (text set, handed to a player entering, refused from a non-owner,
  room gone with the game).

## 8. A joiner backing out deleted the host's game (D8), and related

- Wrong: `GameSession::RemoveMember` lowered the player count whether or not the user was a
  member, and a count of zero deleted the game. A user who had only sent 0x66 or 0x57 could
  also start (and so unlist) the game. A member whose connection dropped kept its place, so
  a game filled up with ghosts. When the host left a game that had members, the game stayed
  listed without a host.
- Changed:
  - Source/Game/GameSession.cpp, `RemoveMember`: counts down only for a member, returns
    false otherwise.
  - Source/Game/GameSessionManager.cpp: `RequestCancel` lets a non-member simply stop
    pointing at the game, and removes the game when its owner leaves; `RequestStart` is for
    the owner only; `OnDisconnectUser` removes a member who was not the owner.
- Established by: the game sends 0x05 and 0x60 whenever it leaves a game, member or not
  (0x3309c8); the rest by reading the server.
- Test: step 21 (back out; start by a non-host refused) and step 22 (three members drop,
  one leaves, places are free again; the game goes when its host leaves). Before the fix:
  both fail.

## 9. A malformed message stopped the server (D9)

Confirmed against the build before the fix: each of these, alone, ended the process: a
string count pointing past the end of the message; an encrypted string with length field 1;
a 0x5D piece claiming 2 GB; **a frame whose length field is 1, 2 or 3**.

- Changed:
  - Include/Common/ByteStream.hpp, Source/Common/ByteStream.cpp: every reader checks the
    bytes left first (`require`), including `read<T>`, the zero-terminated readers, the
    encrypted readers (whose length arithmetic could also go negative) and `read_bytes`
    (which allocated the claimed length before looking). A failed check throws.
  - Source/Lobby Server/LobbyServer.cpp, `HandleRequest`: catches, logs, drops that one
    message; the connection stays. `Run` has a catch of its own around the whole pass.
    `ReadSocket`: a frame length below 4 is refused like one above 2048 (the length counts
    itself, so the payload size came out negative and was used as a 4 GB size).
  - Source/Discovery Server/DiscoveryServer.cpp, `Run`: the same catch around
    `ProcessPacket`.
- Test: step 28 (15 malformed messages on a logged-in connection, each followed by a
  request that must be answered) and step 30 (random bytes, framed and unframed, to all 18
  TCP ports, more behind a login, and to the discovery port). Before the fix the server
  died in step 28. A heavier run outside the test (18,000 random requests behind logins,
  5,000 datagrams, connection floods) also left the server answering.

## 10. Requests on a gateway port: null user (D10)

- Changed: `ResultGetSocialListInitial::Serialize` and `ResultGetSocialListUpdate::Serialize`
  write empty lists when there is no user. Confirmed before the fix: 0x61 to port 40810
  ended the process.
- Test: step 29, which also sends every message id to a gateway port and to a lobby
  connection that never logged in.

## 11. Messages 0x63 and 0x64; the friend lists

- 0x62 and 0x63 are the game's friend list and ignore list requests. The handler for the
  0x62 reply (0x328868, whose assertion text names `m_FriendList`) takes two lists: names,
  and names to mark as online (it calls 0x330cb0 with 1 for each). The handler for 0x63
  (0x328a10, `m_IgnoreList`) takes one. The 0x61 reply handler (0x32ee38) passes its first
  two lists to the first of these and its third to the second.
- Wrong: 0x62 answered friends and ignored users, so ignored names were read as "online";
  the middle list of 0x61 was one placeholder entry; 0x63 had no handler, and the game's
  ignore screen (0x34abb0, 0x34b2b0) would wait 30 seconds for it.
- 0x64 is sent from the login screen with the typed name and password when a login returns
  result 63 (0x339a20). The reply is twelve encrypted strings (0x3841dc case 0x64); the
  game keeps the first (up to 49 characters) and the sixth (up to 15, with `/` forced at
  positions 2 and 5), and its account screen shows them in the e-mail and date-of-birth
  fields (0x32fd08, 0x3354f0). This server never answers a login with 63, so a game has no
  reason to send 0x64.
- Changed:
  - Source/Network/Event/RequestGetSocialListInitial.cpp and
    RequestGetSocialListUpdate.cpp, `Serialize`: second list is the friends who are logged
    in (`UserManager::GetOnlineFriends`, new, Source/Game/RealmUserManager.cpp).
  - New Source/Network/Event/RequestGetIgnoreList.cpp (0x63) and
    RequestGetAccountInfo.cpp (0x64) with headers; `Database::LoadAccount` (new, using the
    existing unused `LoadAccount` statement, which now selects e-mail and date of birth).
    0x64 checks the password like a login and returns result 4 with empty strings otherwise.
- Test: step 19 (0x61, 0x62, 0x63) and step 26 (0x64).

## 12. A console on the server's own network (new setting)

- `report_public_ip_for_lan_consoles` in bin/config.ini, 0 by default. With 1, a console
  observed at a private (10/8, 172.16/12, 192.168/16) or loopback address is reported to
  players whose own address is not private as being at `public_ip`, port unchanged, in the
  game list (0x55), the private-game reply (0x57) and the notification to the host (0x65).
- Changed: `Config::Load` and `Config::ReportedAddress` (new), Source/configuration.cpp;
  `Util::IsPrivateOrLoopbackIPv4`, `Util::IsPublicIPv4` (new), Source/Common/Utility.cpp;
  the three call sites in RequestMatchGame_RTA.cpp, RequestGetGame_RTA.cpp and
  NotifyClientRequestConnect_RTA.cpp (which now takes the recipient's address).
- Limits, also written into config.ini:
  - `public_ip` must be a public IPv4 address in dotted form; the game reads peer addresses
    with `%hd.%hd.%hd.%hd` (0x329078 for 0x55 and 0x57, 0x329ec8 for 0x65), so a host name
    cannot stand in. Otherwise
    the setting is switched off with an error at start.
  - The console on the server's network must itself reach the server at `public_ip`,
    because the gateway hands that address to everyone. On a router without NAT loopback
    it cannot connect at all, with or without this setting.
  - The server cannot see the public port the router gives that console's game socket. It
    works when the router keeps the port, or when UDP 4000 to 4004 (the range the game
    creates its socket with, 0x32a650) is forwarded to the console.
  - Players on the server's network are still told the private address, so that they
    connect directly.
  - A third player learns the others from the game's host, not from the server. If host and
    another player are both on the server's network, the host knows that player by its
    private address and passes that on.
- Test: **not observable by the test client**, which reaches the server from loopback and
  so always counts as local. The decision function was checked with a small program built
  from the same sources (22 cases: off by default; substitution only for an outside
  recipient; unusable `public_ip` switches it off), and the whole test passes with the
  setting on. The wiring of the three call sites was only read.

## 13. Idle connections: what the game sends, and the timeout

Settled from the game:

- The connection object is created with a keep-alive interval of 10 (0x383594 passes 10 and
  10 to 0x381c38, which stores them at +0xa0, connect timeout, and +0xa4). When connected
  and nothing has been sent for more than that many units, the transport sends a frame with
  no payload: four bytes, `00 00 00 04` (0x38b858, state 2, calls 0x38c53c, which calls the
  frame sender 0x38c1e4 with length 0). The time of the last send is stamped in 0x38c314.
- The unit is the lobby clock: lobby-task calls divided by 60 (0x324288), so seconds only
  while the game runs at full speed, and the clock stands still when the task is not called.
- Besides that, while the game-selection window exists and the console is not in a game,
  the game repeats the game search 0x55 every 610 lobby-task calls (0x324288, start of the
  function). The 0x24 "touch session" builder has no caller.

Consequences for the server:

- An empty frame made `ReadSocket` stop processing, so a request behind it in the same read
  waited for the next one. Changed to `continue`. Test: step 27.
- The 30-second limit left 19 seconds of slack for a stalled game (a loading screen). It is
  now the setting `client_timeout`, default 120, minimum 15 (`Config::Load`,
  `LobbyServer::CheckSocketProblem`). Checked with a scratch instance at 15: a silent
  connection was closed after 16 s, one sending an empty frame every 5 s stayed open.
  Whether the lobby task really stalls during a load was not established; this is caution.
- With a longer limit, a console switched off without closing its connection would lock its
  account for that long (a second login got result 1). `RequestLogin::ProcessLoginRTA` now
  lets the new login take over and disconnects the earlier session, and no longer counts
  the asking connection itself. Test: step 31.

## 14. Other things a client could do to the server

- **A client that does not read its replies** made `send()` wait, and one thread serves
  everybody. Client sockets are now non-blocking (`SetSocketNonBlocking`, new in
  Platform.hpp, both platforms; `AcceptConnection`), `ReadSocket` tolerates "would block",
  a failed send closes the connection instead of repeating the error on every pass, and a
  client with more than 1 MB of unsent replies is dropped (`CheckSocketProblem`). Checked
  outside the test: 4,000 unread requests, others answered within 13 ms, offender dropped.
- **More than FD_SETSIZE connections** wrote outside the `fd_set`. `AcceptConnection` now
  refuses a descriptor that does not fit (`SocketFitsSelect`, Platform.hpp). Checked with
  1,300 connections: 302 refused, service back when they closed.
- **Two threads on the same data.** The discovery thread and the lobby thread both use the
  users, games and send buffers; their fields were unguarded. One mutex
  (`LobbyServer::StateMutex`) is now held by whichever is processing a message.
- A removed connection is closed at once (`CheckSocketProblem`) instead of when the last
  reference to its socket goes away.

## 15. Small things

- 0x54: the five numbers of the game info were parsed and thrown away, leaving the
  session's fields indeterminate. `RequestCreatePublicGame_RTA::ParseGameInfo` keeps them.
- Account creation refuses a chat handle longer than 15 characters
  (`RequestCreateAccount::ProcessRequest`): the game refuses a whole member, friend or
  ignore list if one name in it is longer (limit 0xf in 0x3841dc cases 9, 0xc, 0x11, 0x61,
  0x62, 0x63).
- build.sh links the objects of the current sources instead of every `*.o` in the build
  directory. That directory holds 34 stale duplicates named `… 2.o` (as a file-sync client
  leaves them), and one of them broke the link. They were left where they are.
- bin/config.ini: the two new settings, with comments.

## 16. A console that lost its connection could not come back (S1)

Sections 16 to 18 are of 2026-10-11. S1 to S3 are the numbers of
`docs/issues/live-rig.md` 19.12, where the real game was first run against this server (in
an emulator) and these three things were seen or read. The fixes themselves were not run
with the game; their checks are described at the end of section 18.

- Seen with the game (live-rig 19.9): when its connection is gone, the game connects again
  by itself, gateway (0x43) then lobby (0x27), and carries on under the session id it had.
  It does not log in again. On the game list its next request is the periodic 0x55.
- Wrong: a session lived and died with its connection. The user is found by its socket
  (`UserManager::FindUserBySocket`), and `CheckSocketProblem` removed it when the connection
  went, so the game's new connection had a user that was not logged in.
  `RequestMatchGame_RTA::ProcessRequest` sent such a user 0x41, answered with result 0 and
  the list of open games, and closed the connection. The game connected again and asked
  again, every 650 frames or so, until the player dismissed the notice on the screen.
- How the other handlers treat a user that is not logged in: none of them disconnects it.
  0x58 and 0x5E refuse with result 1, 0x5A with 2 because the session id it is sent is
  not the connection's; the others do not look at the login state at all (see "Not
  fixed").
- Changed:
  - Source/Game/RealmUserManager.cpp and its header. `OnConnectionLost` (new): when the
    connection of a logged-in Return to Arms user ends without a logout, the session id,
    the account, the names, the current character and the address the connection came
    from are kept for `client_timeout` seconds. `ResumeSession` (new): the user of a
    connection that is not logged in becomes that session's user again (friend and
    ignore lists are loaded as at a login). `ForgetLostSessions` (new).
  - Source/Lobby Server/LobbyServer.cpp. `CheckSocketProblem` calls `OnConnectionLost`.
    `HandleRequest` calls `ResumeSession` (new) before the handler, for every request
    that starts with a session id: all but 0x06, 0x0F, 0x16, 0x27, 0x42, 0x43 and 0x64
    (the game's header writer 0x37cd14 against 0x37cc64). So the first request after
    the reconnect picks the session up whichever it is: 0x55 on the game list, a
    character save (0x5E) during play.
  - Source/Network/Event/RequestLogin.cpp, `ProcessLoginRTA`: a login ends a lost
    session of the same account, as it already ended a connected one.
  - Source/Network/Event/RequestMatchGame_RTA.cpp and its header: a connection that is
    still not logged in gets result 7 with eight empty arrays. No 0x41, and the
    connection stays open.
- What may pick a session up. The id alone proves little: it travels under a key every
  console has, and the players of a game are told each other's (0x5E, 0x5F). So:
  - only a session whose connection is gone. One that a connection still holds stays
    with that connection, wherever the id comes from;
  - only from the address the lost connection came from, the condition a discovery
    datagram has to meet;
  - only for `client_timeout` seconds, the time a connection that says nothing keeps its
    session; and once;
  - not after a logout (0x17), and not after the account has logged in again.
  Two consoles behind one router have one address. One of them, knowing the other's id,
  can take over the other's session in the moment after that one's connection is lost.
- Why result 7 for the rest. `SRSClient_setResultText` (0x32d498) is called with the result
  by every reply handler of the game (43 call sites; the 0x55 handler at 0x329484, which
  then leaves its list alone when the result is not 0). It picks the text for the result,
  and for four results it also notifies the connect handler (the object at client+0x84)
  with −6, which is what a lost connection (0x32e8b8) and notification 0x41 (0x3301f0)
  end in as well: 7, 1000, 1001 and 1002. On the disc the texts are "Invalid Session ID!"
  for 7 and "The client has timed out", "The server has disconnected", "Not connected to
  the server" for the other three, which the game produces itself. So 7 is the one result
  a server can send that the game takes as "this session is over". The connection is left
  open because closing it is what made the game connect and ask again.
- The eight empty arrays: the game's reply parser reads the arrays whatever the result is
  (0x3841dc case 0x55, from 0x3864c0), passes the result of the header on with them
  (0x3867b4), and reports its own result 45 instead when one is missing (0x386800).
- Not established: what the connect handler does with −6 was not followed (it is called
  through a delegate, 0x3d2350). live-rig 19.9 saw the notice that the connection was
  lost, and from it the way back to the network menu and a normal login.
- Test: steps 1 to 9 of `lobby_fix_checks.py`. Before the fix all nine fail: every list
  request under an old or unknown id gets result 0, the open games, a 0x41 of 14 bytes
  and a closed connection, and a save sent first is refused at once.

## 17. Notification 0x41 was two bytes too long (S2)

- Wrong: `u16` id, `u32` 0, `u32` 0: fourteen bytes with the length. The game reads a
  `u16` where the second `u32` was.
- Changed: `NotifyForcedLogout::Serialize`, Source/Network/Event/NotifyForcedLogout.cpp.
  On the wire now `00 00 00 0c 41 00 00 00 00 00 00 00`.
- Established by: the notification parser 0x387388 reads the id with the two-byte reader
  0x382f8c and a `u32` with 0x383098; its case for 0x41 (0x388654) calls 0x382f8c once
  more and hands the value to `SRSClient_onForcedLogout` (0x3301f0), which overwrites the
  register holding it without reading it. The value has no meaning to the game; 0 is sent.
- Test: step 10 of `lobby_fix_checks.py`, and step 31 of `tools/lobby_e2e_test.py`, which
  finds the 0x41 among the frames of the closed connection.

## 18. A server that had stood still timed everybody out (S3)

- Seen (live-rig 19.9): the computer the server runs on was put to sleep for longer than
  `client_timeout`. When it woke, every client was dropped at once.
- Wrong, twice. `LobbyServer::Run` called `CheckSocketProblem` before it read the
  sockets, so a client whose data was waiting was judged on the time of its previous
  message. And the judgment compared two readings of `std::chrono::steady_clock`, which on
  that computer (macOS) goes on counting while it sleeps: the pause counted as silence of
  the clients, though nobody could have been heard. Whether the clock does the same on
  Windows and Linux was not looked up; the fix does not depend on it. Reading first is
  not enough by itself: the data of a client on another machine is not waiting when the
  server wakes, it comes with that client's next retransmission.
- Changed, Source/Lobby Server/LobbyServer.cpp and its header:
  - `Run`: `CheckSocketProblem` is called after the sockets have been read and written,
    in the same pass.
  - `CheckSocketProblem`: when more than `MAX_SERVER_PAUSE_SECONDS` (5) lie between two
    calls, the server itself did not run in between. That time is added to every
    connection's time of last receipt, but never beyond the present, so a connection
    keeps the idle time it had before the pause and nothing else. A line in the log says
    how long the server stood still.
- The gateway connections are in the same list and loop, so this covers them. The
  discovery thread has no timeout.
- Nothing else in the server measures time, except the new limit of section 16, which is
  deliberately left as it is: a session lost before a long pause is not kept through it.
- Test: step 11 of `lobby_fix_checks.py`. The server process is stopped (SIGSTOP) for
  five seconds longer than its timeout and continued. One client sent a keep-alive frame
  and a request meanwhile, one sent nothing and speaks afterwards, one stays silent for
  good. Wanted: the first is answered, the second keeps its connection, the third is
  dropped one timeout after the server ran again and not before. Run with a timeout of
  15 and of 120 seconds. Before the fix all three were closed when the server ran
  again, the first with its request unread.

**The checks of 16 to 18.** `lobby_fix_checks.py` is a client script of its own, built on
the console of `tools/lobby_e2e_test.py`, because it needs what that test does not ask
for: a server with a short `client_timeout`, a second address of the test machine, and a
server process that may be stopped. All 11 steps pass against the fixed server and all
11 fail against a build of the tree as it was. `tools/lobby_e2e_test.py` passes as before,
16 steps and 33 with `--extended`, also against a build with the address and
undefined-behaviour sanitizers, which reported nothing in either script. These runs were
made with a second instance on ports of its own while the first kept running; the gateway
ports are fixed in the source, so the instance under test was built with other numbers in
`LobbyServer::Start`, and step 30 of the extended test, which goes to 40800–40817 whatever
`--gateway-port` says, was redirected to them.

## 19. A session picked up again was not in its chat room (S6)

Sections 19 and 20 are of 2026-10-11 as well, but after sections 16 to 18 had been run
with the real game (`docs/issues/live-rig.md` section 20). S6 is that section's number
for this one. The fix itself was not run with the game; its check is the test below.

- Seen with the game (live-rig 20.1): its connection was cut on the game list, it came
  back by itself, and its next 0x55 picked the session up (section 16). The game's own
  notice of the lost connection was on the screen, and the way out of that notice is out
  of the lobby: the game sent 0x15 for the public room it had been in, and got result 1.
  It ignores that result.
- Wrong: a lost connection removes its user like any other, and that takes the user out
  of its chat rooms (`ChatRoomManager::OnDisconnectUser`). `ResumeSession` brought back
  the account, the names and the character, but not the room. So the session was there
  again and outside its room: a line said in the room did not reach the console, a line
  of its own was refused (0x22, result 1), a player entering the room was not shown it,
  and its 0x15 failed.
- Changed: Source/Game/RealmUserManager.cpp and its header. `OnConnectionLost` keeps the
  user's public room with the lost session (`m_publicRoomId`, the number a user's public
  room is kept by anyway, read before `RemoveUser` leaves the room). `ResumeSession`
  enters that room again (`ChatRoomManager::JoinRoom`) once the user has its names
  back, unless the connection has entered a public room of its own in the meantime.
- Why the room is put back, and 0x15 not simply answered with 0 for a room the user is
  not in. Either would have made the one reply that was seen come out right. But the
  game does not know that it was gone: it carries on under its session id with the room
  on its screen, does not ask for the rooms or enter one again (after it came back its
  requests were 0x55, and then the 0x15), and leaves the room when it moves on. So the
  session it expects to have is one that is in the room. A server that says yes to the
  0x15 and keeps the user outside leaves everything before the 0x15 wrong, on the
  server's side and for the other players in the room; and it would say yes to any
  console leaving any room it never was in. The result of 0x15 is the least of it.
- Not put back: the chat room of a game. A lost connection ends the user's part in its
  game (section 8: a member gives its place back, a host's game is removed), a session
  that is picked up comes back without a game, and the room belongs to the game. In
  play there is nothing to put back either way: the game's room is closed when the game
  starts, and the consoles leave the public room then (live-rig 19.10).
- A user that really is not in the room still gets result 1 for 0x15, as before: a
  connection that never logged in, a session lost for longer than `client_timeout`.
- Established by: live-rig 20.1 and 20.5 for what the game sends and that it ignores the
  result; the rest by reading the server.
- Test: step 40 of `tools/lobby_e2e_test.py` (new). Two consoles enter the first public
  room; one loses its connection, comes back as the game does and has its session picked
  up by a 0x55. Wanted: a line the other says in the room reaches it, a line of its own
  is accepted, the other, going out and in again, is shown it among the members, and
  its 0x15 gets result 0. Before the fix all four fail, the 0x15 with result 1 as the
  game saw it.

## 20. The Windows build and `std::max`

- Wrong, by reading: `LobbyServer::Run` called `std::max( a, b )` in three places.
  `Include/Platform.hpp` includes `<windows.h>` on Windows, the project does not define
  `NOMINMAX`, and `<windows.h>` then defines `max( a, b )` as a macro. The three calls
  work out the highest descriptor for `select()`, which only the macOS/Linux build
  needs; nothing says they were ever compiled on Windows.
- Changed: Source/Lobby Server/LobbyServer.cpp, `Run`: `std::max< SOCKET >( … )` in the
  three places. A function-like macro is only expanded when its name is followed by a
  parenthesis, so naming the template argument keeps it out. It is how this code base
  writes such calls already (`std::min<size_t>( … )` further down in the same file and
  in RequestGetCharacterData_RTA.cpp, `(std::min)( … )` in PasswordHash.hpp); no other
  plain call of `std::min` or `std::max` is left in Source or Include.
- Checked, short of a Windows compiler: every source the build uses was run through the
  compiler here for syntax with the two macros of `<windows.h>` defined in front of it.
  The tree as it was fails in exactly these three lines; the tree as it is passes.
  **The Windows build was still not compiled.**
- Also: the comment on the minimum of `client_timeout` in Source/configuration.cpp said
  that the game sends a frame at least every 11 seconds. Seen with the real game
  (live-rig 19.4) an idle console sends its empty frame about every 760 frames, some 13
  seconds at full speed and more when the game runs slowly. The comment says so now;
  the minimum of 15 was left. The same sentence is in the comment of bin/config.ini,
  which was not touched (a copy of the server was running from that directory).

**The checks of 19 and 20.** With a second instance on ports of its own, as before:
`tools/lobby_e2e_test.py` passes 16 of 16 and, with `--extended`, 40 of 40 against the
fixed server, also built with the address and undefined-behaviour sanitizers, which
reported nothing. Against a build of the tree as it was before section 19 it passes 39
of 40: step 40 fails. Against a build of the tree as it was before section 16 it passes
32 of 40: step 31 fails on the 14 bytes of the old 0x41, and steps 34 to 40 fail.
`lobby_fix_checks.py`, unchanged, still passes its 11 steps against the fixed server.
The instances under test still had to be built with other gateway numbers in
`LobbyServer::Start`; step 30 now goes where `--gateway-port` points.

## 21 to 30. Hardening for a server that strangers can reach (2026-10-12)

The server now stands on the public internet, and until here it had been made to work
for friends. These sections are the result of reading every handler, the framing, the
stream readers, the crypto wrapper and the database layer with one question: what can
somebody do who is nobody's friend. How it was checked is at the end ("The checks of 21
to 30"); the reproducers are checks of the new `tools/lobby_fuzz.py`, named with each
finding as `abuse: name` or `limits: name`. "Before" results are from a build of the
tree as it was after section 20. Severity is how bad it is for the players of a public
server: **high** harms them or stops the server for everybody, **medium** needs luck or
harms little, **low** is wrong but was not reachable.

What held: the stream readers of section 9. The unfixed tree, built with the address
and undefined-behaviour sanitizers, took 300 fuzz sessions (105,053 messages: every
message id on both games' ports with well-formed and mutated bodies, behind a real login
and without, raw noise, discovery datagrams) without a report and without stopping. No
handler reads past a message, and every SQL statement was already prepared with
parameters. What follows was found by reading and then shown.

## 21. Anybody could overwrite anybody's character (high)

- A stranger could: log in with a throwaway account and send a character save (0x5E)
  naming another player's session id. The server looked that session up among all
  users and wrote the data over the character that player was playing. Being in a game
  with the victim was not needed. `abuse: save` — before: answered 0, the victim's
  character replaced; now: answered 1, character kept.
- And the session id did not have to be learnt: ids came from `std::mt19937` seeded
  once, at start, with one 32-bit number (`UserManager::UserManager`). Two ids of one's
  own are enough to find the seed by trying all of them, and every id the server gives
  out afterwards follows. (Shown by reading only; no search was run.)
- Fixed:
  - Source/Game/RealmUserManager.cpp, `GenerateSessionId`: 64 bits from
    `std::random_device` for every id, checked against the ids in use.
  - Source/Network/Event/RequestSaveCharacter_RTA.cpp: who asks is the connection's
    user; the session id at the front of the request is no longer compared with
    anything. The second id is looked up only among logged-in users, and the save is
    refused unless both have opened or joined the same game: new field
    `RealmUser::m_partyGameId`, set in `GameSession::AddMember` (so for the host when it
    opens the game and for a joiner at its 0x5F), kept when the game leaves the list at
    its start and when a lost session is picked up again (`LostSession`), cleared at
    logout. That is the game's own rule (lobby-protocol.md 8.8): the consoles of a party
    save each other's characters.
  - `FindUserBySessionId` and `FindUserByChatHandle` only find logged-in users, and an
    empty id or handle finds nobody (it used to find the first connection that had not
    logged in).
  - An upload remembers the account and the character it was begun for
    (`CharacterSaveTask::m_targetAccountId`, Source/Game/CharacterSaveManager.cpp),
    instead of reading them from the user object when the last piece arrives.
- Left: a player who is in a game with you can still overwrite the character you are
  playing. That is how the game saves; see SECURITY.md.

## 22. A connection that had not logged in could use the lobby (high)

- A stranger could, without an account: enter chat rooms, speak in them under no name
  (the form the server's own "has joined" lines have), open games and game chat rooms,
  ask for the Champions of Norrath game list with every host's address, and be the
  target of messages addressed to the empty handle. `abuse: no-login`.
- Fixed: `UserManager::FindLoggedInUser` (new) returns the connection's user only if it
  has logged in. Every handler but those of the connect and login sequence (0x06, 0x0F,
  0x16, 0x24, 0x27, 0x42, 0x43, 0x64), logout, the list of public rooms (0x0E, the
  same for everybody) and the upload pieces (0x5D, which need an upload that only a
  logged-in session can begin) uses it, and so gives a connection that has
  not logged in the reply it already gave a connection without a user (the replies of
  section 10, which the game can parse). 0x55 still answers 7, as section 16 needs.
  Source/Network/Event/RequestMatchGame.cpp (0x18) had no user check at all and now
  answers with an empty list.

## 23. Memory without end: uploads, games, chat rooms (high)

- A stranger with one account could make the server keep, for good:
  - **an unfinished character upload per login.** Uploads are kept by session id, a
    second login on the same connection gave a new id, and the old upload stayed with
    its 37 KB. `abuse: relogin`, 3,000 logins on one connection: the process grew by
    219 MB (sanitizer build; 54 MB in a release build). Now 0 MB.
  - **a game per request.** Nothing tied a session to one game. `abuse: games`: 300
    games opened by one session, 304 in everybody's list, and 303 still there after
    the session had gone, each without a host. Asking about another game (0x57, 0x66,
    0x0D, 0x25) after opening one had the same effect: the server forgot which game
    was the user's. Now 1 listed, 0 after it left.
  - **a chat room per request**, 2,000 of them in `abuse: rooms`, every one still
    there afterwards and every one searched for each later room request.
  - **a place in every room entered.** A user was taken out of the last public and
    the last game room it had entered only; in the others its entry stayed for ever.
- Fixed:
  - Source/Game/RealmUserManager.cpp, `EndSession` (new): what `RemoveUser` did to a
    session, without removing the user. `RequestLogin` and `RequestCreateAccount` call
    it when the connection is already logged in, so a second login starts afresh.
  - Source/Game/GameSessionManager.cpp: `LeaveGames` (new) takes a user out of every
    game: games it owns are removed, places in others given back, games without an
    owner removed. Called when a session ends, before it opens a game (one game per
    session) and when it turns to another game. `MayCreateGame`: at most 256 games per
    game type, names and info strings of at most 256 characters (what the game's list
    takes).
  - Source/Game/ChatRoomManager.cpp: `CreateGameChatSession` leaves the session's
    earlier game room first, refuses a room without a name or with more than 256
    characters, and stops at 256 game rooms; `OnDisconnectUser` goes through every
    room. Source/Game/ChatRoom.cpp: members whose user no longer exists are dropped
    (`DropExpiredMembers`), and a room holds 512 at most.
  - Friends and ignored names: 100 each per account (RequestAddFriend.cpp,
    RequestAddIgnore.cpp). The LAN address a console reports about itself is cut at 64
    characters before it is passed on.

## 24. One address could take every connection (high)

- A stranger could: open connections until the server had none left. `limits:
  connections`, before: of 1,500 silent connections 523 were held and 963 refused, which
  is to say that the table was full and nobody else got in; they were held for
  `client_timeout`, and for ever with one byte now and then (`limits: slow-frame`: a
  frame sent a byte every 5 seconds was still being waited for after 150 s, because any
  byte counted as a sign of life). A connection the server had decided to close waited
  without limit for its last replies to be taken.
- Fixed, Source/Lobby Server/LobbyServer.cpp and Include/Network/RealmSocket.hpp:
  - `AcceptConnection`: `max_connections_per_address` (16) and `max_connections` (512,
    and never more than `select()` can watch: `MaxSelectableSockets`, Platform.hpp).
    At the second limit `MakeRoomForConnection` closes the connection that has not
    logged in and has been silent the longest, so that whoever only holds connections
    loses them to the next comer and logged-in players are not touched.
  - `ProcessFrames` (split off `ReadSocket`): `last_recv_time` is set by a whole frame
    only. `CheckSocketProblem`: a connection that has not sent one whole message
    within `first_message_timeout` (30 s) is closed; a console sends 0x43 or 0x27 the
    moment it is connected. A connection being closed gets 5 seconds
    (`MAX_CLOSE_WAIT_SECONDS`) and is no longer read for requests meanwhile. Unread
    replies are capped at 256 KB instead of 1 MB.
  - Now: 16 kept, a console at another address answered in a millisecond meanwhile, the
    16 closed after 29 s; the slow frame closed after 30 s.

## 25. No limit on what one address may ask for (high)

- A stranger could: try passwords as fast as the server answers (`limits: passwords`,
  before: 120 wrong passwords answered in 4 s), create accounts and characters until
  the disk is full (`limits: accounts`: 60 of 60 created; each account can hold twelve
  characters of up to 39 KB), make the server write a log line per message for as long
  as it liked, and keep its one thread busy with requests that cost a password hash or
  a database write each.
- Fixed:
  - A budget per address (Include/Common/RateLimit.hpp, `LobbyServer::ProcessFrames`,
    `RequestCost`): `requests_per_second` (50) units a second up to `request_burst`
    (400); a request costs 1, a chat line 2, a game or room opened or a friend-list
    change 5, a login or a character download 10, an account 20, a character written
    30 more (`AddRequestCost`). **Nothing is refused**: a connection whose address has
    used its budget is not read until there is budget again, so the game sees a slow
    server and no new error. The connections of one address take turns. `limits:
    budget`: of 3,000 requests at once 553 were answered in 10 s, the rest waited, and
    a console at another address was answered in 1 ms meanwhile.
  - Password checks (0x16, 0x64): 40 per address at once, one more every 3 s, right or
    wrong. Now 46 of the 120 were answered in 20 s. Accounts: 20 per address at once,
    one more every 3 minutes (`AllowAccountCreation`; refused with the result a taken
    name gets). Now 19 of 60 (one had been used by the check before).
  - Source/Database/Database.cpp, `LimitSize`: `PRAGMA max_page_count` from
    `max_database_mb` (1024). Tried with 16: after 816 characters the next was refused
    ("database or disk is full", result 1 to the console), the file stood at 16 MB and
    the server went on.
  - Source/logging.cpp: 300 lines of a kind at once and 10 a second after that; what
    was left out is counted in the next line. A line is cut at 600 characters.
  - Discovery datagrams: one per session every 100 ms is acted on
    (DiscoveryServer.cpp); each used to send the host a message. The server never
    answers a datagram by datagram, so there is nothing to amplify.
  - The limits per address are for strangers: unless `limits_for_local_addresses=1`
    they leave alone 127.0.0.0/8 and the private ranges (10/8, 172.16/12, 192.168/16),
    so a test rig on the server's machine or its network runs as before. That matters:
    `lobby_e2e_test.py --extended` floods on purpose, and with the limits applied to
    its address it fails from step 30 on (tried: 3 steps of 40 on a first run, 8 on a
    second straight after).

## 26. Memory the server had not set was sent to another console (medium)

- `RealmUser::m_localPort` was never given a value. A joiner that asks for a game by
  name (0x57 carries no port) and then sends its datagram made the server send the host
  notification 0x65 with that field as it lay in memory: four bytes of whatever the
  allocation held before. `abuse: port` — on the sanitizer build the host received
  0xbebebebe, the pattern that build fills new memory with. Now 0.
- Fixed: Include/Game/RealmUser.hpp, the field starts at 0. The new fields of
  `RealmSocket` are set where they are declared for the same reason.

## 27. Somebody else's game and room (medium)

- A Champions of Norrath console that had only asked about a game (0x0D or 0x25) could
  send new data for it (0x44), player counts included: `abuse: game-data` made a game
  count as full. Fixed in RequestUpdateGameData.cpp: only the host.
- Starting a game closed whatever chat room had the game's name, and anybody can make a
  room under any free name. Fixed in RequestStartGame.cpp: only the host's own room.

## 28. Passwords (medium)

- How they were kept: PBKDF2-HMAC-SHA256 with 1,000 rounds and a 32-byte salt, as
  `pbkdf2$rounds$salt$hash`. Not plain, but 1,000 rounds is little against somebody
  who has the database file; the salt came from a generator seeded with 32 bits; the
  comparison stopped at the first wrong byte; and a name that does not exist was
  answered without doing the work, so the time said which names exist.
- Changed, Include/Crypto/PasswordHash.hpp, Source/Crypto/PasswordHash.cpp,
  Source/Database/Database.cpp:
  - 50,000 rounds (`PASSWORD_ROUNDS`), 16 salt bytes from `std::random_device`. The
    number stays this low because the check runs on the one thread that serves
    everybody: about 50 ms here. PBKDF2 keeps the two hash states that have taken in
    the key pads instead of rebuilding them every round, which halves the work per
    round; 300 combinations of password, salt, rounds and length give the same bytes as
    the old code.
  - **Existing databases keep working.** The rounds are part of what is stored, so an
    old entry checks as before, and `VerifyAccount` stores the password again with the
    current rounds after a successful check (`UpdatePassword`, new statement). Tried:
    an account made by the unfixed build (`pbkdf2$1000$…`) logged in on the fixed one,
    was `pbkdf2$50000$…` afterwards, and logged in again; a wrong password was refused
    before and after.
  - The comparison looks at every byte; an unknown name costs one check against a
    decoy; a stored entry is not trusted to ask for more than 2,000,000 rounds.
  - No new dependency: the tree's own SHA-256.
- Not changed: on the wire a password is as good as plain (section "Not fixed").

## 29. Names in the log, and in the database (low)

- Names, room names and chat handles went into log lines as sent. A line break in one
  started a line of the sender's making, an escape sequence reached the terminal of
  whoever read the log. Source/logging.cpp: characters below 0x20 and 0x7F are written
  as `?`.
- A zero inside an account name ended the name in the database (bound as a C string)
  but not in the server's memory. RequestCreateAccount.cpp: `VerifyUserData`, which
  existed and was never called, now refuses control characters and fields longer than
  32 (name), 64 (password), 128 (e-mail), 32 (date of birth) characters;
  `VerifyAccount` binds the name with its length.
- `VerifyAccount` built strings from columns without asking whether they were null, and
  left its statement in the middle of a query. Both changed. Transaction.hpp uses
  `std::string` and includes it now.

## 30. Small things found on the way (low)

- `Util::round_up` and `round_down` called `abs()` on values that can be the smallest
  integer and added past the largest; both are undefined. Only the readers for
  encrypted strings without a block length call them, and no handler uses those
  readers, so no message reached this. Utility.cpp and ByteStream.cpp work in 64 bits.
- `LobbyServer::Run`: `CheckSocketProblem` ran outside the `try` of the loop, so an
  exception while a connection was being closed would have ended the process; and the
  frame length was read through a cast to `int32_t *`. Both changed.
- `LobbyServer::Start` went on when a lobby port could not be opened, and `Run` then
  used a null pointer. It returns, and `main` ends the program as it does for a
  gateway port.
- Source/configuration.cpp: a setting that is not a number ended the server with an
  uncaught exception at start. `ReadNumber` keeps the default and says so.
- New settings `con_gateway_port` and `rta_gateway_port` (first of the eight gateway
  ports of each game, 40800 and 40810 as before): a second instance for testing no
  longer needs its own build (the note at the end of section 20).
- Includes for what is used were added where a file relied on another header to bring
  them (`<cstdint>`, `<stdexcept>`, `<algorithm>`, `<string>`, `<vector>`).

**The checks of 21 to 30.** All against instances of their own, on ports of their own on
127.0.0.1, started from scratch directories; the server in `bin/` was not touched.

- `tools/lobby_e2e_test.py --extended` and `tools/lobby_fix_checks.py` (both in the
  reverse-engineering repository, both unchanged): 40 of 40 and 11 of 11 against the
  fixed tree built with the address and undefined-behaviour sanitizers, which reported
  nothing; 16 of 16 and 40 of 40 against a build of this tree with the flags of
  `build.sh`, made into a scratch directory; 40 of 40 against a build with the thread
  sanitizer, which reported nothing either (there are two threads, section 14).
- `tools/lobby_fuzz.py` (new, in this repository; Python 3, standard library only,
  127.0.0.1 only; SECURITY.md says how to run it):
  - `--mode fuzz`: against the fixed tree with the address and undefined-behaviour
    sanitizers, set to end the process at the first report, four runs: seed 2, 2,914
    sessions and 677,933 messages in 25 minutes (before the changes to chat rooms,
    discovery and 0x18 were in); seeds 3 and 4, 2,386 and 2,254 sessions, 664,076 and
    554,499 messages, 20 minutes each (before the limits per address were made to
    leave private addresses alone, one line of `AcceptConnection`); and seed 7 on the
    code as it is, 1,485 sessions and 381,786 messages in 12 minutes. 2.28 million
    messages in all, no report, and the server answered after every session. With the
    thread sanitizer (code as for seeds 3 and 4): 156 sessions, 34,714 messages, no
    report. One report did come up on the way and was of this work's own making: a
    field added to `RealmSocket` and not set (section 26, last sentence).
  - `--mode abuse`: the ten checks named above. Against the unfixed tree seven fail
    (port, save, no-login, game-data, relogin, games, rooms; chat, datagrams and
    no-reader pass: sections 9 and 14 hold). Against the fixed tree all ten pass.
  - `--mode limits`, against an instance started with `limits_for_local_addresses=1` and with
    the machine's LAN address as the second address: the five checks pass. Against the
    unfixed tree slow-frame, connections, passwords and accounts fail.
- Every source the build uses was run through GCC 15 with its own standard library
  (`-std=c++20 -fsyntax-only`, the only GCC at hand; macOS, so the POSIX side of
  Platform.hpp) without an error. **Not compiled on Linux, and not with MSVC.** No new
  source file: Include/Common/RateLimit.hpp is a header, listed in the project files.
- bin/config.ini was not touched (a server was running from that directory). The new
  settings all have defaults; SECURITY.md lists them.

## 31. A relay that speaks for many players (new setting, 2026-10-12)

- The players of the browser build reach the server through a relay that holds their
  connections. Where that relay connects from the server's own public address, the
  limits per address of sections 24 and 25 take all its players for one stranger: 16
  connections, one request budget, one allowance of password checks, and 20 new accounts
  and then one every three minutes, for everybody together.
- New setting `trusted_addresses` (config.ini; empty by default): up to 16 IPv4
  addresses that the limits per address leave alone in the way they leave loop-back and
  private addresses alone, whatever `limits_for_local_addresses` says.
  Source/configuration.cpp (`ReadAddressList`, `Config::ExemptFromAddressLimits`),
  Include/configuration.hpp, `Util::IPv4ToNumber` (Source/Common/Utility.cpp), and the
  one line of `LobbyServer::AcceptConnection` that decides whether a connection is
  exempt. An entry that is not four numbers with dots (a name, a range, 0.0.0.0, a
  number over 255) is left out with an error in the log and is never read as something
  wider. The server names every listed address in a warning each time it starts.
- **Everything that connects from a listed address shares no limit at all.**
  SECURITY.md, "A relay that speaks for many players", says what that gives away, what
  the relay of the reverse-engineering repository does in the server's place today
  (connections: yes; requests, password checks and accounts: no) and what it would have
  to do. Nothing else changes: with the list empty the server decides as before.
- Checked against instances of their own on ports of their own on 127.0.0.1, started
  from scratch directories, built with the flags of `build.sh` into a scratch directory;
  the server in `bin/` was not touched. The machine was busy with other work throughout
  (load average 90 to 500).
  - Without the setting: `lobby_e2e_test.py` 16 of 16 and `--extended` 40 of 40,
    `lobby_fix_checks.py` 11 of 11 (an instance with `client_timeout=15`),
    `lobby_fuzz.py --mode abuse` 10 of 10; and with `limits_for_local_addresses=1`,
    `--mode limits` 5 of 5 (16 connections kept, 46 of 120 wrong passwords answered in
    20 s, 19 of 60 accounts, 557 of 3,000 requests in 10 s).
  - With `limits_for_local_addresses=1` and `trusted_addresses=127.0.0.1`, from
    127.0.0.1: 16 of 16, 40 of 40, 11 of 11 and 10 of 10. (With that first setting and
    no list the extended test fails from step 30 on, as section 25 says: 8 of 40 steps
    here.) `--mode limits` from 127.0.0.1 shows the limits per address gone: 512 of
    1,500 silent connections held until the first-message time-out, 60 of 60 accounts,
    3,000 of 3,000 requests answered in 10 s, and 120 logins taken at once, of which 23
    were answered after 20 s while a newcomer got no answer within 10 s; slow-frame,
    which is not a limit per address, passes. The same mode from the machine's LAN
    address, which is not on the list (the fuzzer's sockets bound to that address by a
    wrapper), passes 5 of 5: 16 connections, 46 logins in 20 s, 19 accounts, 552
    requests.
  - The same two settings on a build with the address and undefined-behaviour
    sanitizers: 16 of 16, 40 of 40 and nine of the ten abuse checks, no report. The
    tenth, relogin (3,000 logins), was stopped by hand: under the sanitizers on that
    busy machine it had not finished after ten minutes.
  - A list holding a name, a range, 0.0.0.0, 300.1.1.1 and one address twice: the four
    are refused in the log, the address is listed once.
  - After the last edit (an include) the tree was built again and 16 of 16 and 40 of 40
    were run once more, with the list empty and with 127.0.0.1 on it, together with the
    accounts and budget checks from both addresses.
  - Not compiled on Linux, with GCC or with MSVC. No new source file; `<algorithm>`,
    `<cstddef>` and `<vector>` are the only new includes.

## Changes to the test client

- It can be started as documented again: Python 3.13+ `argparse` imports the standard
  `dis`, which `tools/dis.py` shadowed.
- Character uploads are sent as the game sends them (see 5); the old test waited for a
  reply to every message, which is what the server used to do, not what the game expects.
- 0x61 and 0x62 are read as the game reads them (see 11).
- New steps 22 and 26 to 31, and more checks inside 17 to 19, 21, 23 and 25.

Added with section 19:

- Seven steps behind the two logouts, 34 to 40, each with consoles and accounts of its
  own, so that steps 1 to 33 are what they were. 34 to 39 are the six checks of
  `lobby_fix_checks.py` that need no hold on the server process: a session picked up by
  0x55 and by a character save first, and an id nobody has, the id of a session that
  logged out, of one replaced by a newer login and of one a connection still holds, each
  to be answered with result 7 and nothing else (section 16). 40 is the chat room of
  section 19. The checks that need a short `client_timeout`, a second address or a
  server that may be stopped stay in `lobby_fix_checks.py`.
- Step 31 compares the 0x41 it provokes with the twelve bytes of section 17, and wants it
  to be the last thing on the closed connection, instead of only looking for its id.
- Step 30 takes the sixteen gateway ports from `--gateway-port` (Return to Arms from that
  number, Champions of Norrath from ten below) instead of always going to 40800–40817.
  With the default it does what it did. The test now sends nothing to a port it was not
  given or handed by the server, so a second instance can be tested beside a running one
  without redirecting anything.

## Not fixed, not verified

- Nothing was tried with the real game. Every reply layout above is from reading its parser.
- The Windows build was not compiled. Champions of Norrath paths were not tested; its
  requests go through the same stricter readers, so a CON request shorter than what this
  server's parser reads is now dropped where it used to be read as zeros.
- The LAN setting (12) was not exercised end to end.
- The server still requires a discovery datagram to come from the same IP address as the
  user's TCP connection. Not changed: it is the only check on that datagram.
- Two consoles behind one router, with the server elsewhere, are told each other's public
  address and depend on the router looping traffic back (`docs/lobby-protocol.md` 9.3). Not
  changed: it needs a decision about when to hand out LAN addresses instead.
- The short text per game (array 2 of the 0x55 reply, first string of the 0x57 reply, fixed
  "Test") was left. The game stores it at +4 of its game record and can sort the list by it
  (0x329440, 0x32dc40); what it shows was not found. The Champions of Norrath handlers of
  this server put the owner's name in the corresponding place.
- The filler in 0x0E (two strings and two numbers per room; the game keeps a second string
  of up to 127 characters and two 16-bit numbers per room, 0x328b70) and the zeros in 0x0F
  were left. 0x11 still ignores the room name; 0x44 still expects CON's layout.
- A taken game name still gives result 2 (the project's enum has 38 for it). The texts
  behind the result codes were not available, so no code was changed on a guess.
- Chat handles are still not unique. Two accounts with one handle confuse friend lists and
  instant messages.
- Any logged-in user who knows another's session id can overwrite that user's current
  character (0x5E names the target by session id; the character id in the request is still
  ignored). In a game every player has the others' session ids. This is how the game saves:
  one machine is picked to save a given player's character (0x1eb5c8).
- The value after the result in the 0x23 reply is sent as 0; its meaning is unknown.
- Unknown message ids still get no reply. Error logging is not rate-limited: a flood of bad
  messages produces one log line each.
- Notification 0x2F (friend status) is still sent to the wrong set of users, but the game's
  handler for it is an empty function (0x330028), so it does not matter.

Added with sections 16 to 18:

- None of the three was tried with the real game, and the Windows build was again not
  compiled (no new source files, nothing POSIX-only; the new code keeps clear of
  `std::min` and `std::max`, which need care under `<windows.h>`).
- A request from a connection that is not logged in is still handled by most handlers as
  if it were: only 0x55 (result 7), 0x58 and 0x5E (result 1) and 0x5A (result 2) refuse
  it. Such a connection can open a game (tried: 0x54 gets result 0), enter a chat room
  without a name, or ask for the characters of account −1. Checking the login in one
  place needs a refusal the game can parse for every request, since its parser reads the
  body whatever the result is. Whether 0x58 and 0x5E should answer 7 as well was left
  open: during play the game would then be told "fatal" where it is now told that a save
  failed, and what it does with that was not seen.
- A console whose connection has died without the server noticing (the server learns of
  it by the timeout, or by a failed send) comes back while the old connection still
  holds the session. It is told 7 until the old connection has timed out; logging in
  again takes the account over at once.
- A character save for another player (0x5E naming that player's session) still fails
  while that player's connection is lost and has not come back.
- The texts behind the result codes are on the disc (string keys `clientText1` to
  `clientText68`; 0x32d498 picks the key). Results 0 to 36 use the key numbered one
  higher, 40 to 63 the key numbered two lower, 37 to 39 and 500 to 503 the texts about
  login name, password, date of birth and e-mail. So result 1, this server's general
  failure, reads "Timed out"; 2, which a taken game name gets, "A database error
  occured"; 38, the project's `GAME_NAME_IN_USE`, "Invalid Birthdate"; 40 "This session
  already has a game"; 46 "Invalid game name"; and 45, the game's own result for a reply
  it cannot parse, "A protocol violation has occurred". No code was changed for this.

Added with sections 19 and 20:

- "Nothing was tried with the real game" above no longer holds for the Return to Arms
  path that `docs/issues/live-rig.md` sections 19 and 20 ran: account, log in, lobby
  characters, host, list, join, start, saves in play, and sections 16 to 18 (in an
  emulator, Play!; no console). On that path no reply was refused or misread by the
  game, and the fixes of sections 3, 5, 6 and 7 were each seen to do what they were made
  for. It still holds for private games, a list of two or more games, friends, ignore,
  messages, cancelling, deleting a character, the LAN setting, and Champions of Norrath.
- Section 19 was not tried with the real game, and the Windows build was again not
  compiled.
- A session lost while its console sat in a game's room before the start comes back
  without its game and without the game's chat room. If the game then asks to leave that
  room, it is told 1, as it was for the public room before section 19. What the game
  does in that situation was not seen; keeping a game through a lost connection would
  be the fix, and is a larger change.
- A console whose last request before the loss was a 0x15 that never arrived is put
  back into a room it believes it has left. Should it then enter that room again on the
  same session, 0x0C answers 1 ("already in it") although it is in the room; should it
  enter another, the server has it in both and takes it out of only the newer one when
  it goes. Seen from the game neither can happen in the lobby, where a lost connection
  leaves only the way out (live-rig 20.1); in play it would take a connection lost in
  the moment the game starts. Answering 0x0C with the room for a user that is already
  in it, and leaving the old public room on entering a new one, would close both; not
  done here, because it changes 0x0C for everybody.
- While a user is gone, the consoles already in its room are not told, and not told
  when it is back either: this server sends no notification about who is in a public
  room (the game parses several, 0x2A to 0x3C, whose meanings were not worked out).
- bin/config.ini still says that a console sends an empty frame "every 11 seconds or
  so" (section 20).
- The minimum of `client_timeout`, 15 seconds, leaves about two seconds over the 760
  frames between two empty frames of an idle game at full speed (live-rig 19.4), and
  nothing when the game runs slower than that. The default of 120 is not affected.

Added with sections 21 to 30:

- None of it was tried with the real game. What a console sends was taken from
  `docs/lobby-protocol.md` and the two test tools, which pass unchanged; the limits are
  far above what a console does there, but "a console never does this" is read, not
  seen, for: logging in twice on one connection, opening a second game or game room
  without closing the first, entering a game's room after turning to another game.
- **The save rule of section 21 rests on one unverified step.** A joiner becomes a
  member of a game, and so of its party, by its own 0x5F; the host is not asked. With
  the real game the joiner sent 0x5F (lobby-protocol.md 8.5), so real parties are
  covered. If some path into a game exists that sends no 0x5F, saves of the other
  players' characters by that console, and of its character by theirs, would now be
  refused with result 1, where they used to be stored.
- A lost session keeps its party, but a save for a player whose connection is lost and
  has not come back still fails, as before (section "Added with sections 16 to 18").
- The protocol's encryption is one key that is on every disc. Passwords and session ids
  can be read by anyone on the path, who can then log in as that player; nothing here
  can change that without changing the game.
- The limits are per address. Players behind one router share them (ample for a
  household), many addresses together are not limited as a whole, and nothing is
  limited per account. Private and loopback addresses are not limited per address at
  all unless `limits_for_local_addresses=1`: a server that strangers reach through a
  proxy or a NAT that gives them such an address has to set it, and raise the limits.
- The Windows build watches 46 connections at most (`FD_SETSIZE` is 64 there and 18 are
  listeners); `max_connections` is lowered to that with a warning at start. It was
  that many before, without the warning.
- Database writes are still done on the serving thread with SQLite's defaults, one
  `fsync` per write. The budget of section 25 bounds how many one address can cause;
  it does not make them cheap.
- Chat handles are still not unique. E-mail address and date of birth are stored as
  typed. There is no way to change a password.
- Not reviewed: Dependency/sqlite, the body of Source/Crypto/rijndael.cpp beyond its
  entry points (third-party AES; it is given whole blocks only), and the files the
  build leaves out (Request_5F.cpp, Request_61.cpp, RequestGetFriendList.cpp,
  Notify_4C.cpp, Notify_65.cpp, NotifyClientReqConnect.cpp,
  NotifyReserveUserSlot_RTA.cpp). RealmCharacter.cpp's parser is commented out and
  nothing a client sends reaches what is left of it.

## Found wrong or missing in docs/lobby-protocol.md

(The document itself was not edited when this list was written. On 2026-10-11, with
section 19, it was brought in line with these points and with what the real game did in
`docs/issues/live-rig.md` sections 13, 19 and 20.)

- Section 2 and 13: the idle keep-alive is not unknown; see 13 above.
- Section 2: a reply is matched by tracking number and parsed by the type of the *pending
  request*, not by the id in the reply (0x381f80).
- Section 2: a frame length of 1 to 3 did not close the connection, it killed the server.
- Section 7: 0x62 is the friend list (names, names online), not "friends, ignored"; the
  middle list of 0x61 is the friends who are online; 0x63 and 0x64 are as in 11 above;
  0x19 sets the room text.
- Section 7 and 8.3: 0x5D has no reply in the game's eyes, and the reply to 0x58/0x5E
  belongs after the last piece; "result (sent at once)" describes the server, and is what
  made a correct `u32` impossible. The `u32` of 0x58 is the id the game uses for later saves.
- Section 8.3: when the data fits the first message the game sends one more piece, empty
  and flagged. The old server never stored such a character.
- Section 10, D3: what the game does with the second login string is in 3 above.
- Section 10, D9 and D10: tried here against the old build; both are real.
- Notification table: 0x2F is parsed and then ignored by the game.
- Notification table (added with 17): the body of 0x41 is one `u16`, which is what the
  server sends now.
- Section 7 (added with 16): 0x55 is no longer "always 0"; a connection without a login
  gets 7. And "the server identifies the user by the TCP connection and ignores the
  session it is sent" has an exception now: on a connection without a login the session
  id of the request is looked up among the sessions whose connection was lost.
- Section 2 (added with 16): the result codes and their texts, as in "Not fixed" above;
  result 7 is the one the game treats as the end of its session.
