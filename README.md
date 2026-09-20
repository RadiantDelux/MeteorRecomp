# MeteorRecomp

MeteorRecomp is an experimental static-recompilation project for **Dragon Ball Z: Budokai Tenkaichi 3 (Europe)** on Wii, revision **RDSPAF**.

The project translates the game's PowerPC executable ahead of time to native C++, then links it with a Wii runtime/HLE layer and the Aurora graphics backend. The goal is to preserve original guest behavior while replacing console hardware interfaces with host implementations where required.

> [!IMPORTANT]
> MeteorRecomp does **not** include the game, a disc image, extracted game files, Nintendo keys, the original DOL, or generated translated game code. You must provide your own legally obtained copy locally.

## Status

MeteorRecomp is under active development and is **not release-ready**. Compatibility work is focused on accurate behavior rather than bypassing game logic or forcing progression.

The public tree contains the Meteor project manifest, validated function-boundary data, title-specific runtime bindings, the static translator, the shared runtime, and the renderer required to build the project.

## Supported revision

| Property | Value |
| --- | --- |
| Title | Dragon Ball Z: Budokai Tenkaichi 3 |
| Region | Europe / PAL |
| Game ID | `RDSPAF` |
| Entry point | `0x8000403C` |
| SDA / r13 | `0x80631320` |
| SDA2 / r2 | `0x806338C0` |
| main.dol SHA-256 | `004f206f760447655e5f4ac8ff554290ea573b186afe9c1f9c331f1b5786b16d` |

Other revisions are not currently supported by the supplied manifest.

## Repository layout

- `projects/meteor/` — MeteorRecomp manifest, function map, CMake presets, and title-specific native bindings.
- `translator/` — PowerPC static translator and code-generation pipeline.
- `runtime/` — host runtime, Wii HLE services, memory, audio, input, storage, scheduling, and platform integration.
- `aurora-main/` — graphics backend used by the runtime.
- `benchmarks/` — translator benchmark support.

Build products, generated translation output, logs, diagnostics, extracted game files, and local development data are intentionally excluded from Git.

## Requirements

- .NET 8 SDK
- CMake 3.25 or newer
- Ninja
- Clang / LLVM
- On Windows, an LLVM-MinGW toolchain is recommended

## Game data

Extract your own copy of the supported game revision and place the DOL at:

```text
local-data/meteor/DATA/sys/main.dol
```

The manifest validates the DOL SHA-256 before translation. Do not commit extracted game data.

## Build

Build the translator:

```powershell
dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release
$translator = 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
```

Generate the Meteor translation:

```powershell
dotnet $translator translate-recursive 0x8000403C `
  --project projects/meteor/recomp.yml `
  --outdir build/meteor/generated/functions `
  --output-metadata build/meteor/generated/base_translation_output.json `
  --threads 12 `
  --prune-stale

dotnet $translator generate-data-init `
  --project projects/meteor/recomp.yml

dotnet $translator emit-build-shards `
  --project projects/meteor/recomp.yml `
  --base-metadata build/meteor/generated/base_translation_output.json
```

Then build the native executable:

```powershell
cmake --preset release -S projects/meteor
cmake --build --preset release -S projects/meteor --parallel 4
```

The configured output directory is:

```text
build/meteor/native/<host>/bin/
```

## Development principles

- Preserve retail guest semantics whenever practical.
- Do not patch generated C++ as a permanent fix; generated output must remain reproducible.
- Keep title-specific address bindings under `projects/meteor/runtime_native/`.
- Keep reusable Wii hardware/runtime behavior in the shared runtime.
- Validate indirect function targets and function boundaries before adding them to the translation set.
- Do not add copyrighted game data, extracted assets, or generated game code to the repository.

## License

MeteorRecomp is distributed under the **GNU GPL v3**. See [`LICENSE`](LICENSE).

Third-party components retain their respective licenses and copyright notices; see [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

This project is not affiliated with, endorsed by, or associated with Nintendo, Bandai Namco, Spike, or the original game's publishers.
