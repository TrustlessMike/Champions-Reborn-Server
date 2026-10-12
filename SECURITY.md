# Security of this server

This is a hobby server for two PlayStation 2 games. It speaks the games' own lobby
protocol, which cannot be changed without changing the games, and that sets what can and
cannot be protected. What was found and fixed when the server went on the public
internet is in [FIXES.md](FIXES.md), sections 21 to 30.

## What is protected

- **The process.** No message, however malformed, should be able to stop the server or
  make it read or write memory it should not. Every reader checks the bytes left in the
  message first; a message that does not parse is dropped and the connection goes on.
  The build with the address and undefined-behaviour sanitizers runs the fuzzer (below)
  without a report.
- **Accounts and characters.** A request acts for the account its *connection* logged in
  as, never for a session id written in the request. A connection that has not logged in
  can do nothing but connect, create an account, log in and ask for the rules and the
  statistics. A character can be written by its owner and by the consoles that are in a
  game with its owner (that is how the game saves, see "not protected"), by nobody else.
  Session ids are 64 bits from the system's random source.
- **Passwords at rest.** Stored as PBKDF2-HMAC-SHA256, 50,000 rounds, 16 random bytes of
  salt each (`Include/Crypto/PasswordHash.hpp`). A password stored by an earlier version
  (1,000 rounds) still works and is stored again in the new form at its owner's next
  login. Checks take the same time whether the name exists or not.
- **The database.** Every statement is prepared with parameters. The file cannot grow
  beyond `max_database_mb`.
- **Availability, against one address.** The limits below. The server is one thread and
  can be overwhelmed by many addresses at once like anything else on the internet.

## Limits

Settings (config.ini; the value shown is the default):

| setting | default | what it limits |
| --- | --- | --- |
| `max_connections` | 512 | client connections in all. At the limit a newcomer replaces the connection that has not logged in and has been silent the longest; players who are logged in are never the ones to go. (A Windows build watches 46 at most: `FD_SETSIZE`.) |
| `max_connections_per_address` | 16 | connections one address may hold |
| `first_message_timeout` | 30 | seconds a new connection has to send its first whole message |
| `client_timeout` | 120 | seconds without a whole message before a connection is dropped. A message arriving a byte at a time does not count. |
| `requests_per_second`, `request_burst` | 50, 400 | the budget of one address: a request costs 1 unit, a chat line 2, a game or room opened or a friend-list change 5, a login or a character download 10, an account created 20, a character written 30 more. An address that has used its budget is not read until there is budget again: its requests wait, nothing is refused. |
| `max_database_mb` | 1024 | size of the database file |
| `limits_for_local_addresses` | 0 | the limits per address are for strangers: unless this is 1 they leave alone the addresses only the server's own machine and network can have (127.0.0.0/8, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16) |

Fixed in the code: 40 password checks per address at once and one more every 3 seconds
(logins and account-detail requests, right or wrong); 20 accounts created per address
at once and one more every 3 minutes; one game and one game chat room per session, 256
of each in all; 100 friends and 100 ignored names per account; 12 characters per
account; frames of at most 2,048 bytes; 256 KB of unread replies per connection; 5
seconds for a connection the server is closing to take its last replies; one discovery
datagram per session every 100 ms; 300 log lines of a kind at once and 10 a second
after that (what is left out is counted in the log).

A console never comes near any of these. Several consoles behind one router share that
router's address and so its budget, which is ample for a houseful.

Two things to check when the server is set up:

- **Where do strangers appear to come from?** If the server is reached through
  something that gives every player an address of its own network (a proxy, some
  container networks, a NAT in front of the machine), the server sees private addresses
  and, by default, limits none of them. Set `limits_for_local_addresses=1` there, and
  raise the limits per address, since all players then share one allowance. The limits
  that are not per address (connections in all, one game and room per session, the
  database, the log, the time-outs) hold either way.
- **The protocol test floods on purpose.** `lobby_e2e_test.py --extended` sends some
  thousand messages and logs in dozens of times within a minute. From 127.0.0.1 or from
  the server's own network it runs as before; from an address the limits apply to, its
  steps 30 and after are slowed until they fail, which is the limits working.

## What is not protected, and cannot be from here

- **Nothing on the wire is secret.** The "encryption" of the protocol is AES with one
  key that is on every game disc. Anyone who can see a player's traffic can read the
  password and the session id, and can take the session over. Do not use a password here
  that is used anywhere else.
- **The people you play with.** Consoles talk to each other directly, so the players of
  a game learn each other's addresses. The game also lets every console of a party save
  every other member's character; a server that refused that would lose saves. So a
  player in your game can overwrite the character you are playing. The server cannot
  check that somebody who reports having joined a game (0x5F) really did; it only
  requires that the two are in the same game and that the saver knows the other's
  session id, which the game hands to the party and to nobody else.
- **Accounts are free.** There is no e-mail check, and no way to change or recover a
  password. E-mail address and date of birth, which the game asks for, are stored as
  typed. Chat handles are not unique.
- **A discovery datagram** is accepted from the address a session's connection comes
  from. Somebody who knows a session id and can forge that source address can give a
  wrong port for it and spoil that player's joining of games.
- IPv4 only. No limit is kept per account, only per address and per connection.

## The fuzzer

`tools/lobby_fuzz.py` (Python 3, standard library only) talks to 127.0.0.1 and nothing
else. Start an instance of your own from a scratch directory, on ports of its own, and
never point it at a server people are playing on:

```
c++ ... -fsanitize=address,undefined -fno-sanitize-recover=all ...   # see build.sh for the rest
mkdir /tmp/lobby && cd /tmp/lobby && cp <built binary> champions-server
printf 'service_ip=127.0.0.1\npublic_ip=127.0.0.1\ncon_gateway_port=43800\nrta_gateway_port=43810\ncon_lobby_port=43900\nrta_lobby_port=43910\ndiscovery_port=11301\n' > config.ini
./champions-server &
python3 tools/lobby_fuzz.py --gateway-port 43810 --minutes 30 --pid $!     # fuzz
python3 tools/lobby_fuzz.py --gateway-port 43810 --mode abuse --pid $!      # one check per finding
```

A session that ends the server is named, and `--seed S --only-session N` runs it again.
`--mode limits` checks the limits above and needs a second instance started with
`limits_for_local_addresses=1` (and, to show that one address does not hold up another,
`--other-address` with this machine's LAN address). The protocol test of the
reverse-engineering repository (`tools/lobby_e2e_test.py --extended --gateway-port N`,
`tools/lobby_fix_checks.py`) must pass against the same build.

## Reporting a problem

Open an issue on the repository of this fork. If it is something that would let a
stranger harm the players of a running server, say only that in the issue and ask for a
private contact; the maintainer will give one. Please include the message bytes or the
fuzzer's seed and session number.
