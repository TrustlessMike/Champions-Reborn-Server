### About this fork

This fork of [HikikoMarmy/Champions-Reborn-Server](https://github.com/HikikoMarmy/Champions-Reborn-Server)
adds, on branch `macos-port`:

- a macOS / Linux build (`sh build.sh`, then `sh run.sh`; needs a C++20 compiler, nothing else);
- fixes for defects found by testing the server against the lobby protocol as the games speak
  it — see [FIXES.md](FIXES.md) for each one, how the right behaviour was established, and the
  test that covers it.

The protocol notes and the two-client test used here live in
[champions-return-to-arms-re](https://github.com/TrustlessMike/champions-return-to-arms-re)
(`docs/lobby-protocol.md`, `tools/lobby_e2e_test.py`). Return to Arms itself has been run
against this fork, in an emulator (Play!) and not on a console: account, log in, lobby
characters, hosting, joining, launch, play with its saves, and a lost connection
(`docs/issues/live-rig.md` sections 19 and 20 there; FIXES.md sections 16 to 19 come from
those runs). Champions of Norrath has not been run against it, and the Windows build was
not compiled after these changes.

---

### Champions Reborn Emulated Server
A replacement server aimed at restoring **online gameplay** for **Champions of Norrath** and **Champions: Return To Arms** for the **PlayStation 2**.

The server is currently a work in progress.

## Support:
| Game                          | Status             |
|-------------------------------|--------------------|
| Champions of Norrath          | ✅ Supported       |
| Champions: Return To Arms     | ✅ Supported       |

---

## 🔧 Dependencies

- [SQLite3](https://www.sqlite.org/index.html) — Used for session and user data storage
