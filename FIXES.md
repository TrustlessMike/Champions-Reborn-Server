# Fixes to the Champions Reborn server

Changes made on 2026-10-10 to the working tree in `server/upstream` (branch `macos-port`,
nothing committed), on top of the macOS/Linux port. They are meant to let a group of friends
play Return to Arms through this server.

How to read this file:

- "The game" is the Return to Arms executable SLUS_209.73. An address such as 0x3841dc is a
  function in it; that is where the correct behaviour was read. Nothing here was run against
  a real console or an emulator.
- "Before the fix" results come from running the new test against a build of the tree as it
  was before these changes (rebuilt in a scratch directory from `git archive` plus the port's
  diff). That build failed 17 of the 33 steps and its process died in step 28.
- The test is `tools/lobby_e2e_test.py`. `python3 tools/lobby_e2e_test.py 127.0.0.1` runs
  16 steps, `--extended` 33. Both pass in full against the fixed server, also with
  `report_public_ip_for_lan_consoles=1`, and also against a build with the address and
  undefined-behaviour sanitizers, which reported nothing.
- D1 to D10 are the defect numbers of `docs/lobby-protocol.md` section 10.

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

## Changes to the test client

- It can be started as documented again: Python 3.13+ `argparse` imports the standard
  `dis`, which `tools/dis.py` shadowed.
- Character uploads are sent as the game sends them (see 5); the old test waited for a
  reply to every message, which is what the server used to do, not what the game expects.
- 0x61 and 0x62 are read as the game reads them (see 11).
- New steps 22 and 26 to 31, and more checks inside 17 to 19, 21, 23 and 25.

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

## Found wrong or missing in docs/lobby-protocol.md

(The document itself was not edited.)

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
