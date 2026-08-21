# Moreno.LogiPro

Moreno.LogiPro is a native Windows C++ library, optional CLI, and optional Win32 GUI for reading and configuring compatible Logitech HID++ devices without Logitech G HUB.

The first hardware target is the Logitech G Pro Wireless M-R0070 through its Lightspeed receiver. The receiver and onboard-profile workflow has been exercised against VID/PID `046D:C539`.

## Status

This is an early, hardware-backed project.

- `logipro.dll` contains HID enumeration, HID++, onboard-profile, lighting, capture, and host-binding logic.
- `logipro.exe` is an optional CLI client of the DLL.
- `logipro_gui` is an optional GTK4 client of the same DLL.
- HID++ discovery, onboard-profile reads, CRC-checked button writes, backup/restore, and onboard lighting disable are implemented for the current target.
- Support for other Logitech models is not implied by the current target.

## Build

Requires CMake 3.25 or newer and a C++20 toolchain. Building the optional GUI also requires GTK4 development files and `pkg-config`.

```powershell
cmake -S . -B build -DLOGIPRO_BUILD_CLI=ON -DLOGIPRO_BUILD_GUI=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Set either option to `OFF` when only the DLL or one client is needed. The generated build directory is intentionally ignored by Git.

## CLI

Run the executable from the generated build directory:

```powershell
.\bin\logipro.exe --probe-hidpp
.\bin\logipro.exe --profile-bind 7 F13
.\bin\logipro.exe --profile-restore
.\bin\logipro.exe --profile-lighting-off
```

`--probe-hidpp` is read-only. Profile writes back up the active sector in the current working directory, update only the requested button or lighting records, recompute the sector CRC, and verify the readback. Restore preserves the current lighting records so restoring a button profile does not unexpectedly re-enable an earlier lighting configuration.

Host-side bindings are temporary while the process runs:

```powershell
.\bin\logipro.exe --bind-back F15 --bind-forward F16
```

`--capture-hid` and `--watch-buttons` are read-only diagnostics. They require live input while running and stop with `Ctrl+C`.

## GUI

The GUI is a small GTK4 client with refresh and onboard-lighting controls. It links against `logipro.dll` on Windows or the platform library on other systems; it does not duplicate HID++ logic.

```powershell
.\bin\logipro_gui.exe
```

The CLI remains independently usable for diagnostics and automation.

## Architecture

```text
logipro.dll
├── Windows HID enumeration and transport
├── HID++ feature discovery and device reads
├── onboard profile storage and CRC verification
├── lighting and button operations
└── optional capture and host-side input bindings
     ├── logipro.exe      CLI client
     └── logipro_gui      GTK4 GUI client
```

Public headers live in `include/logipro`. Implementation is organized by domain under `src`: `cli` contains presentation and argument parsing, `hid` contains Windows HID transport and capture, `hidpp` contains protocol/device logic, and `input` contains raw-input and host-hook handling. GUI code is in `gui`. The exported interface is a C++ API intended for clients built with the project toolchain.

```text
src/
├── cli/       optional command-line client
├── hid/       Windows HID enumeration and report capture
├── hidpp/     HID++ transport, feature discovery, profiles, lighting
└── input/     raw input and host-side mouse hooks
gui/           optional GTK4 client
include/       exported library headers
```

## Safety

Profile commands write device state. Keep the generated `logipro-backup-sector-*.bin` file until the result is confirmed. The backup is local runtime data and is excluded from Git. Do not commit device backups, HID paths, binaries, or machine-specific build files.

Host-side bindings install a global low-level mouse hook and suppress selected buttons while active. Stop the process before disconnecting the mouse or receiver.

## References

- [LogiTux](https://github.com/libratbag/libratbag)
- [OpenLogi](https://github.com/AprilNEA/OpenLogi)
- [Solaar HID++ implementation](https://github.com/pwr-Solaar/Solaar/blob/master/lib/logitech_receiver/hidpp20.py)
- [Logitech G Pro lighting support](https://support.logi.com/hc/en-gb/articles/360023183694-Customize-lighting-settings-on-the-G-PRO-gaming-mouse)

## License

Moreno.LogiPro is released under the [MIT License](LICENSE).
