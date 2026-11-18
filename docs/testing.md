# Automated tests

KEMU uses QtTest and CTest for automated regression testing. Test sources reside in `autotests/` and run directly from the build tree without installing KEMU or launching a VM.

## Build and run

Configure a development build with tests enabled, then run the full suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

List available tests or run a specific suite:

```sh
ctest --test-dir build -N
ctest --test-dir build -R '^dbusdisplaytest$' --output-on-failure
```

When `BUILD_TESTING` is enabled, the guest agent (`agent/`) is built natively for its unit tests (`agent…test`). See `agent/README.md` for end-to-end tests, which require a guest-like environment.

In CI—or any environment without Podman or Docker—configure with `-DKEMU_AGENT_BUNDLE=OFF`; otherwise, CMake configuration stops because the `kemu-agent` guest bundle is built in a container (the agent's own tests still run). Pipelines producing bundles require a job with a container engine (plus QEMU user-mode emulation when targeting another architecture) running `cmake --build build --target agent-bundle`.

Use `ctest --test-dir build -V` for verbose output, or `cmake --build build --target dbusdisplaytest` to build only the D-Bus viewer test. Standard KEMU development dependencies and Qt6 Test are required.

## Test suites

| Test                    | Coverage                                                                                                      |
| ----------------------- | ------------------------------------------------------------------------------------------------------------- |
| `ovfparsertest`         | OVF descriptors, hardware and disk metadata, manifests, malformed documents, and size conversions             |
| `templatestoretest`     | Template precedence, validation, cloud-init lookup, user templates, and hidden IDs                            |
| `templatepackagetest`   | Template package round-trips, JSON imports, archive paths, and invalid packages                               |
| `linkedclonestest`      | Clone backing references, frozen source VMs, and orphan detection                                             |
| `diskimagechecktest`    | Disk formats, backing/data files, path restrictions, and unsafe image references                              |
| `diskencryptiontest`    | Secret file descriptors, command-line options, password errors, and secret erasure                            |
| `tpmstatetest`          | Encrypted TPM-state detection, key descriptors, and rekeying                                                  |
| `dbusdisplaytest`       | D-Bus display protocol, framebuffer updates, DMA-BUF descriptors, cursor, audio buffers, and acknowledgements |
| `controlconnectiontest` | Asynchronous D-Bus control-connection teardown, pending calls, and file-descriptor ownership                  |

### D-Bus viewer tests

`dbusdisplaytest` uses a local peer-to-peer GDBus connection to exercise `QemuDisplayListener` without launching QEMU. Its 13 test cases cover software and mapped scanouts, stride and buffer validation, clipped updates, short backing files, dirty regions, `Disable`, cursor and mouse events, DMA-BUF geometry and FD ownership, deferred update replies, audio jitter prebuffering, and PCM ring-buffer wraparound.

`controlconnectiontest` exercises the asynchronous control-connection lifecycle without running QEMU. Its two test cases cover destruction during the D-Bus handshake and teardown with pending method calls, including FD ownership and suppression of callbacks after their owner is destroyed. Run both D-Bus suites with:

```sh
ctest --test-dir build -R '^(dbusdisplaytest|controlconnectiontest)$' --output-on-failure
```

These suites validate protocol handling and buffer management; they are **not** end-to-end graphics tests. EGL/DMA-BUF rendering, Wayland subsurfaces, pointer capture, and reconnecting to a live QEMU instance still require manual or integration testing in a suitable desktop session.

## External tools

Most tests require no running services, though some suites invoke external host tools:

- `qemu-img`: Real disk-image checks and encrypted image verification (skipped when missing).
- `swtpm` and `swtpm_ioctl`: TPM-state rekeying (skipped if unavailable).
- `tar`: Required by `templatepackagetest` for archive fixtures.

QtTest reports skipped cases explicitly, so a passing CTest run does not guarantee that every optional integration test was executed.
