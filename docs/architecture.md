# Architecture

LogiPro uses a library-first layout influenced by LogiTux/libratbag’s driver-oriented source tree and OpenLogi’s separation of transport, HID++ features, and clients.

```text
include/logipro/       public library API
src/hid/               Windows HID discovery and report transport
src/hidpp/             HID++ requests, feature discovery, profiles, lighting
src/input/             Raw Input and host-side mouse hooks
src/cli/               optional CLI argument parsing and presentation
gui/                   optional GTK4 client
```

The `logipro_core` shared library owns device access and protocol behavior. The CLI and GUI link to that target and do not duplicate HID++ implementation. New protocol features belong under `src/hidpp`; OS-specific transport belongs under `src/hid` or `src/input`; client-specific formatting belongs in `src/cli` or `gui`.

Reference projects:

- [LogiTux/libratbag](https://github.com/libratbag/libratbag)
- [OpenLogi](https://github.com/AprilNEA/OpenLogi)
