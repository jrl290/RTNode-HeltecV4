# Native unit tests

Host-side tests for `RemoteConfigCore.h` (the platform-independent core of the `REMOTE_CONFIG` feature): the partial
configuration apply, the admin-list rules (`super_admin`, `admins_add` / `admins_remove`) and the USB frame receive buffer.

```
make -C tests/native test
```

Needs `g++` (C++17) and ArduinoJson (`ARDUINOJSON=<path to its src>`; defaults to the copy PlatformIO downloaded).
No board, no Reticulum, no Arduino core: the real `ROM.h` and `FirewallMode.h` are compiled against a fake EEPROM, so the
tests also check what is actually persisted.

Deliberately breaking a rule (for instance letting `admins_remove` delete slot 0) makes the suite fail; that is how the
security rules were checked.
