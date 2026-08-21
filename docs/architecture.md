# Architecture

LogiPro uses a library-first layout influenced by LogiTux/libratbag’s driver-oriented source tree and OpenLogi’s separation of transport, HID++ features, and clients.

```text
include/logipro/api.h  public C ABI library API
src/api/               C ABI adapter and library debug state
src/hid/               Windows HID discovery and report transport
src/hidpp/             HID++ requests, feature discovery, profiles, lighting
src/input/             Raw Input and host-side mouse hooks
src/cli/               CLI argument parsing and presentation
gui/                   GTK4 application and platform entrypoint
```

The `logipro_core` shared library owns device access and protocol behavior and is published as `logipro.dll` or `liblogipro.so`. Its only exported contract is the C ABI in `include/logipro/api.h`. The single `logipro.exe` application links the library, opens the GTK4 GUI with no command flags, and runs CLI presentation when command flags are supplied. New protocol features belong under `src/hidpp`; OS-specific transport belongs under `src/hid` or `src/input`; client formatting belongs in `src/cli` or `gui`.

Reference projects:

- [LogiTux/libratbag](https://github.com/libratbag/libratbag)
- [OpenLogi](https://github.com/AprilNEA/OpenLogi)
