# Moreno.LogiPro

Moreno.LogiPro is a modular C++ application and C ABI DLL for reading and configuring compatible Logitech HID++ devices without Logitech G HUB. The first hardware target is the Logitech G Pro Wireless M-R0070 through its Lightspeed receiver. The receiver and onboard-profile workflow has been exercised against VID/PID `046D:C539`.

## Status

This is an early, hardware-backed project.

- `logipro.dll` exposes the device and control surface through the C ABI in `include/logipro/api.h`.
- `logipro.exe` is the only application executable: it opens the GTK4 GUI by default and dispatches CLI commands when command flags are supplied.
- The GUI and CLI both call the DLL; neither contains duplicate HID++ transport logic.
- HID++ discovery, onboard-profile reads, CRC-checked button writes, backup/restore, and onboard lighting disable are implemented for the current target.
- Battery telemetry is exposed through the C ABI and GUI; this mouse reports voltage, so its displayed percentage is an estimate.
- The GTK interface separates Overview, Sensitivity, and Onboard Mapping tabs; sensitivity supports live DPI changes and five persistent onboard DPI levels when the device exposes them.
- Support for other Logitech models is not implied by the current target.

## Build

Requires CMake 3.25 or newer, a C++20 toolchain, GTK4 development files, and `pkg-config`.

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Build output is written to the project-root `bin/` directory. `bin/`, `build/`, and `lib/` are ignored by Git.

## CLI

Run the same executable with a command flag for diagnostics or automation:

```powershell
.\bin\logipro.exe --probe-hidpp
.\bin\logipro.exe --profile-bind 7 F13
.\bin\logipro.exe --profile-restore
.\bin\logipro.exe --profile-lighting-off
.\bin\logipro.exe --bind-back F15 --bind-forward F16
```

With no command flag, the GTK4 interface opens. `--debug` enables library diagnostics only when a command is supplied or when the GUI is started from an existing terminal; LogiPro never allocates a new console.

`--probe-hidpp` is read-only. Profile writes back up the active sector in the current working directory, update only the requested button or lighting records, recompute the sector CRC, and verify the readback. Restore preserves the current lighting records so restoring a button profile does not unexpectedly re-enable an earlier lighting configuration.

Host-side bindings are temporary while the process runs. `--capture-hid` and `--watch-buttons` are read-only diagnostics that require live input and stop with `Ctrl+C`.

## GUI

The GTK4 interface presents structured device, battery, onboard-profile, lighting, sensitivity, and button-map views with asynchronous refresh and controls. It links against `logipro.dll` on Windows or the platform library on other systems; it does not display raw probe text or duplicate HID++ logic.

```powershell
.\bin\logipro.exe
```

## Architecture

```text
logipro.dll
├── Windows HID enumeration and transport
├── HID++ feature discovery and device reads
├── onboard profile storage and CRC verification
├── lighting and button operations
├── capture and host-side input bindings
└── C ABI in include/logipro/api.h
logipro.exe
├── GTK4 GUI (default)
└── CLI dispatch for command flags
```

The public DLL contract is the C-compatible `include/logipro/api.h`; it uses opaque snapshots, fixed-width integers, caller-provided buffers, and status codes. Implementation is organized by domain under `src`: `api` adapts the C contract, `cli` contains command parsing and presentation, `hid` contains HID transport and capture, `hidpp` contains protocol/device logic, and `input` contains raw-input and host-hook handling. GTK code is in `gui`.

```text
src/
├── api/       C ABI adapter and library debug state
├── cli/       CLI dispatch compiled into the application
├── hid/       Windows HID enumeration and report capture
├── hidpp/     HID++ transport, feature discovery, profiles, lighting
└── input/     raw input and host-side mouse hooks
gui/           GTK4 application and platform entrypoint
include/       C ABI header and internal C++ model headers
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
