# Changelog

## Unreleased

- Added runtime `standalone`, `master`, and `slave` role configuration.
- Made slave instances read-only for client writes and exposed the active role
  through Redis `HELLO` and `INFO` responses.
- Added the slave-to-master handshake, a managed receive thread, clean shutdown,
  and live master link status reporting.
- Added length-prefixed RESP command reception and sequential command application
  on the slave replication thread.
- Added Master-side Slave registration, write-event framing, non-blocking
  replication broadcast, and live connected-Slave reporting.
- Added up to three Slave connection attempts at startup and after a broken
  replication link. Resynchronization of missed writes is not included.
- Reorganized source code into public headers, store, network, tests, clients, and third-party directories.
- Updated the Makefile to build from the structured source layout.
- Separated the public network API from Reactor-only connection details.
- Moved allocator and memory-pool implementation into `src/memory/`.
- Moved all project object files and executables under `build/obj/` and `build/bin/`.
