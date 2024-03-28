# Building with Bazel (recommended)

```shell
# Files are copied to out/{branch}/dist
$ tools/bazel run --config=raviole //private/devices/google/raviole:gs101_raviole_dist
```

See `build/kernel/kleaf/README.md` for details.

## Disable LTO

**Note**: This only works on `raviole-mainline` branch.

```shell
# Files are copied to out/{branch}/dist
$ tools/bazel run --lto=none --config=raviole //private/devices/google/raviole:gs101_raviole_dist
```

# ABI monitoring with Bazel (recommended)

**Note**: ABI monitoring is not supported on `raviole-mainline` branch.

```shell
# Compare ABI and build files for distribution
$ tools/bazel build --config=raviole //private/devices/google/raviole:gs101_raviole_abi

# Update symbol list aosp/android/abi_gki_aarch64_pixel
$ tools/bazel run --config=raviole //private/devices/google/raviole:gs101_raviole_abi_update_symbol_list

# Update ABI aosp/android/abi_gki_aarch64.xml
$ tools/bazel run //aosp:kernel_aarch64_abi_update

# Copy files to distribution
$ tools/bazel run --config=raviole //private/devices/google/raviole:gs101_raviole_abi_dist
```
