# Versions and the manifest

An installer needs to know what the keyboard runs and what a build holds. Then it can skip what is already in place, and say which keymap the keyboard runs. The Neuron's firmware carries the version of this module, the keymap repository it was built from and that repository's commit. Each half reports its own program's CRC32 and module version, over whichever link it is on. Every build writes `manifest.json` beside its images.

## What a build embeds

`scripts/build` works out three values and passes them to CMake as `DEFY_BUILD_MODULE_VERSION`, `DEFY_BUILD_SOURCE` and `DEFY_BUILD_COMMIT`. The module's `CMakeLists.txt` writes them into the generated header `<dygma/defy_build_info.h>` as string macros of the same names. Only the files that include the header recompile when the values change.

| Value | Where it comes from | At most |
| --- | --- | ---: |
| Module version | `git describe --tags --always --dirty --abbrev=7` in this module's checkout. A west workspace may hold the module without its tags; then, while the module is at the revision west checked out, the revision that the config directory's `west.yml` names for `zmk-defy-wireless`, with describe's `-dirty`. A revision that is a commit hash keeps describe's abbreviated hash. `unknown` when the module is not a git checkout | 32 bytes |
| Source | The config directory's repository as `owner/repo`, from its `origin` remote, read the same way from SSH and HTTPS URLs. `local` when the config directory is not in a git repository or has no `origin` | 140 bytes |
| Commit | The config directory's repository's `HEAD` in full, with `-dirty` when its tracked files have changes, as describe counts them. Empty outside git | 72 bytes |

The values are printable ASCII without `"` or `\`; `scripts/build` refuses a build whose values break this or the lengths above. A build without them, such as a plain `west build`, gets `unknown`, `local` and an empty commit. An installer reads the three values from a running Neuron with the [half updater](half-updater.md#versions)'s VERSION request.

The Neuron's targets embed all three. The halves embed only the module version, and their source and commit stay empty, so a commit in a keymap repository leaves the halves' images byte-identical unless it changes their settings in `config/defy.conf`. CI, a remote machine through `scripts/remote` and a local build compute the same values from the same commits, so their images stay byte-identical too. The fixed abbreviation keeps the hash the same length in a shallow clone and a full one. `scripts/remote` copies the module's tree with its `.git` to the remote machine, uncommitted changes included, so it computes the same values. A build through `scripts/zmk` works the values out on the host. A build run inside ZMK's container, as in CI, works them out there, and tells git to trust the module's and the config directory's checkouts, which a mounted folder often gives another owner.

## The halves' report

A half running ZMK reports what it runs in a 28-byte record, little-endian:

| Bytes | Field |
| --- | --- |
| 0 | Format, 1 |
| 1 | Side: 0 left, 1 right |
| 2..5 | The program's CRC32 from the half's own seal at flash offset `0x5000`; 0 without a valid seal |
| 6..9 | The seal's version; 0 without a valid seal |
| 10..27 | The module version: ASCII, padded with zero bytes, cut to its first 18 bytes |

The CRC32 is the one a package's seal carries, which the [half updater](half-updater.md)'s PROBE reads from the factory bootloader and `manifest.json` lists. A half compares nothing itself; it only reports. `include/dygma/defy_half_version.h` encodes and decodes the record.

The record travels by both links:

- **Bluetooth.** The control service on each half has a third characteristic, `69a897d6-87cf-4e08-9eb2-973bbf71b003`, read only, with encryption. Its value is the record. It is declared right after the command characteristic, and the Neuron looks for it there: its discovery goes one characteristic past the command's and stops. New characteristics therefore go after this one. The Neuron reads the record once after each connection, when it discovers the service, after the wake report and any battery read that is due. It retries a failed read after 1 s while the connection lasts.
- **Cable.** A `DV` frame carries the record in bytes 2..29, between the magic and the CRC16, in place of an idle keep-alive, as `DB` frames carry battery telemetry. The half sends one at its first idle frame after the wired link comes up, then one every 60 s. A lighting acknowledgement and battery telemetry that are due at the same frame go first, so at start-up the report follows the battery report by a frame. The Neuron accepts a `DV` frame only from a connected port whose side matches the record's.

A half whose firmware lacks the report has neither and stays unknown. Its discovery costs the Neuron one more request and no reads.

The Neuron keeps one record per side for each link, with when it arrived, and drops a record when its link goes down for that side. HALF VERSIONS gives the newer of a side's two records. A half usually holds both links at once, so pulling its cable leaves the Bluetooth record in place, which the Neuron would not read again until the next connection. A half that sleeps, is switched off, or is reset by OPEN, loses both links and reports again before it counts as known. The [half updater](half-updater.md#versions)'s HALF VERSIONS reads these records.

## manifest.json

`scripts/build` writes `manifest.json` into its output directory after a batch, when that directory holds `neuron-hybrid.hex` and a pair of half images. The pair is the one the batch built, and the `-hybrid-radio` pair when it built both. A batch that built neither pair takes `left-hybrid-radio.bin` and `right-hybrid-radio.bin` when both exist, otherwise `left-hybrid.bin` and `right-hybrid.bin`. Without those three files it removes a stale `manifest.json` instead. The run's artifact, a local build and every release therefore carry it. When the batch did not build `neuron-hybrid`, the values come from the `manifest.json` that already lists that Neuron image; without one, it writes no manifest.

The manifest lists `neuron-hybrid.hex`, the UF2 loader and ZMK together, which the installer writes. It does not list `neuron-hybrid.uf2`, ZMK alone for the [UF2 loader](uf2-loader.md)'s drive, which the installer does not use.

```json
{
  "schema": 1,
  "module": "v0.1.0",
  "source": "borovaka/zmk-defy-wireless",
  "commit": "2826cb2...",
  "commit_time": "2026-10-24T12:05:31+03:00",
  "min_installer": "v0.1.0",
  "files": {
    "neuron": {"name": "neuron-hybrid.hex", "size": 285712, "sha256": "..."},
    "left": {"name": "left-hybrid-radio.bin", "size": 471040, "sha256": "...",
             "program_crc32": 3221225472, "seal_version": 1},
    "right": {"name": "right-hybrid-radio.bin", "size": 471040, "sha256": "...",
              "program_crc32": 1073741824, "seal_version": 1}
  }
}
```

| Field | Meaning |
| --- | --- |
| `schema` | The manifest's format, 1. An installer refuses a schema it does not know |
| `module`, `source`, `commit` | The values the Neuron image embeds |
| `commit_time` | The commit's own time in ISO 8601 with its offset, from `git log -1 --format=%ci`, so two builds of one commit write the same manifest. UTC is `+00:00`, which newer git's `%cI` writes as `Z`. Empty outside git |
| `min_installer` | The oldest installer that can write these images. `scripts/build` holds it as a constant, raised when the images need something an older installer lacks |
| `files` | Each image's file name, size and SHA-256 in lowercase hex. The halves add their seal's program CRC32 and version, as numbers |

The keys are written in this order with two-space indentation and a final newline, so the file is stable byte for byte. The SHA-256 sums catch a broken download. They are not a signature, since the manifest comes from the same place as the images.
