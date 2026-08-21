# Contributing

## Before changing code

Open an issue for substantial protocol or architecture changes. Small fixes and documentation improvements can be proposed directly with a pull request.

## Development

Build and test on Windows with:

```powershell
cmake -S . -B build -DLOGIPRO_BUILD_CLI=ON -DLOGIPRO_BUILD_GUI=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Keep the DLL, CLI, and GUI boundaries intact. Put HID++ transport and device behavior in the library; clients should format output or handle presentation. Do not commit generated build output, device backups, HID paths, or credentials.

Hardware-specific changes must identify the device model, connection path, feature IDs, request/response evidence, and whether the result was verified live. Do not describe inference as protocol fact.

## Pull requests

Include a concise problem statement, the validation commands, and any hardware limitations. Keep unrelated formatting or refactoring out of the change. New write operations must preserve the existing backup and readback safeguards.
