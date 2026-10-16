# ECB package tool

Python 3 standard library only: no installation or third-party packages.
Run `uv run tools/pkg/ecb_pkg.py ...` or `python tools/pkg/ecb_pkg.py ...` with
an installed Python 3 interpreter. Wire specification and failure/reboot behavior:
[docs/package-format.md](../../docs/package-format.md).

## Build and inspect

From the repository root (one-line commands work in PowerShell):

```sh
uv run tools/pkg/ecb_pkg.py build --brain brain/build/brain_ota.bin --node examples/bench_app/build/bench_app.bin -o update.ecbpkg
uv run tools/pkg/ecb_pkg.py info update.ecbpkg
```

Node-only, with explicitly provisioned IDs and a product-name check:

```sh
uv run tools/pkg/ecb_pkg.py build --node "app.bin,cmd=0x701,reply=0x702,name=axis-left" --node "app.bin,cmd=0x703,reply=0x704,name=axis-right" -o nodes.ecbpkg
```

`--brain` is optional; `--node` can appear up to eight times. At least one
entry is required. Brain-only packages are allowed. The builder puts BRAIN first,
then nodes in argument order. Paths/names with spaces need quotes; commas are
option separators and are not supported inside paths/names. IDs accept decimal
or `0x` hexadecimal, defaulting to `0x79E/0x79F`. Names are ASCII, at most 32 bytes;
empty/omitted disables the name check. Use **both IDs** from `/node/scan` for a
provisioned node. Standard ID zero is legal and is not a default sentinel.

The tool rejects non-ESP32-S3 images, bootloaders/merged images (app descriptor
required), truncated/unaligned image segments, trailing padding/signatures,
invalid IDs/names, empty packages, too many nodes and capacity overflows. Input
must be an exact unsigned ESP-IDF application `.bin`. The envelope check is not
proof the firmware will boot: ESP-IDF verifies the full image in its destination
slot. BRAIN must be a **Phase-5-capable brain_ota build**, not just any S3 app.

Limits: BRAIN 3,145,728 bytes; each NODE 1,835,008 bytes; entire package 8,257,536
bytes including manifest/alignment. Eight maximum-size nodes plus a maximum-size
brain do not fit. `info` rechecks the manifest/ranges and every payload SHA-256,
then prints entry metadata as JSON. Errors print `ecb_pkg: ...` to stderr and exit
2; successful commands exit 0. No signing or encryption is performed.

## Upload

Install the brain's new partition table over USB once (bench guide section 11),
then use Ethernet:

```sh
curl.exe -i --data-binary "@update.ecbpkg" http://<ip>/package
curl.exe http://<ip>/package/status
```

202 means staged and checked, **not finished**. Poll until `done`/`failed`, and
inspect individual node results even for `done`. The brain may reboot first.
Only the GPIO3-wired node can be automatically recovered if not already parked.
Power loss during relay causes the next boot to replay the package from its first
node. All HTTP commands are unauthenticated: trusted bench LAN only.

## Parser interoperability vector

`tests/vectors/sample.ecbpkg` is a 352-byte package built by this tool, containing
one BRAIN and one NODE named `bench-node` (default IDs). Both payloads are the same
64-byte **synthetic envelope**, not executable firmware: zero-filled, byte 0 =
`0xE9`, byte 1 = 1, LE16 chip ID at 12 = 9, LE32 first-segment length at 28 = 16,
LE32 app descriptor magic at 32 = `0xABCD5432`, byte 63 = `0xEF ^ 0x32 ^ 0x54 ^
0xCD ^ 0xAB`. All other bytes remain zero. It has no appended hash. Do not flash it.
To reproduce, write those bytes to a scratch `sample.bin`, then:

```sh
uv run tools/pkg/ecb_pkg.py build --brain <scratch>/sample.bin --node "<scratch>/sample.bin,name=bench-node" -o <scratch>/sample.ecbpkg
```

`test_update_pkg` parses the committed vector in C. It can additionally validate
the manifest of a real tool-built package, without loading its payload into RAM:

```sh
cmake -S tests -B build-host-p5
cmake --build build-host-p5 --config Debug
ctest --test-dir build-host-p5 -C Debug --output-on-failure
build-host-p5/Debug/test_update_pkg.exe update.ecbpkg
```

That last command validates the manifest/ranges only (C parser contract); pair it
with `ecb_pkg.py info` for payload SHA verification.
