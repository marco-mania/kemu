# Automated tests

KEMU uses QtTest and CTest for its automated regression tests. The test sources
are in `autotests/`; tests run from the build tree without installing KEMU or
starting a virtual machine.

## Build and run

Configure a development build with tests enabled, then run the complete suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

List registered tests or run an individual suite:

```sh
ctest --test-dir build -N
ctest --test-dir build -R '^dbusdisplaytest$' --output-on-failure
```

Use `ctest --test-dir build -V` for verbose output. Building just the D-Bus
viewer test is also possible with `cmake --build build --target dbusdisplaytest`.
The build requires the normal KEMU development dependencies plus Qt6 Test.

## Test suites

| Test | Coverage |
| ---- | -------- |
| `ovfparsertest` | OVF descriptors, hardware and disk metadata, manifests, malformed documents and size conversions |
| `templatestoretest` | Template precedence, validation, cloud-init lookup, user templates and hidden IDs |
| `templatepackagetest` | Template package round-trips, JSON imports, archive paths and invalid packages |
| `linkedclonestest` | Clone backing references, frozen source VMs and orphan detection |
| `diskimagechecktest` | Disk formats, backing/data files, path restrictions and unsafe image references |
| `diskencryptiontest` | Secret file descriptors, command-line options, password errors and secret erasure |
| `tpmstatetest` | Encrypted TPM-state detection, key descriptors and rekeying |
| `dbusdisplaytest` | D-Bus display protocol, framebuffer updates, DMA-BUF descriptors, cursor, audio buffers and acknowledgements |

### D-Bus viewer tests

`dbusdisplaytest` uses a local peer-to-peer GDBus connection to exercise the
`QemuDisplayListener` methods without launching QEMU. Its 13 test cases cover
software and mapped scanouts, stride and buffer validation, clipped updates,
short backing files, dirty regions, `Disable`, cursor and mouse events,
DMA-BUF geometry and FD ownership, deferred update replies, audio jitter
prebuffering and PCM ring-buffer wraparound.

These are protocol and buffer regression tests, **not** end-to-end graphics
tests. EGL/DMA-BUF rendering, Wayland subsurfaces, pointer capture and real
QEMU reconnect behaviour still require manual or integration testing on a
suitable desktop session.

## External tools

Most tests need no running services. A few suites invoke host utilities:

- `qemu-img`: real disk-image checks and encrypted image verification; these
  cases are skipped when the tool is missing.
- `swtpm` and `swtpm_ioctl`: TPM-state rekeying; skipped if unavailable.
- `tar`: required by `templatepackagetest` for archive fixtures.

Skipped cases are reported by QtTest; a passing CTest suite does not imply
that every optional integration case ran.
