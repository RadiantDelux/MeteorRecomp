# MeteorRecomp

<p align="center">
  <a href="#building-from-source"><img alt="Windows 10 / 11, x64" src="https://img.shields.io/badge/Windows-10%20%2F%2011%20%C2%B7%20x64-0078D4"></a>
  <a href="#what-it-does"><img alt="PowerPC static recompilation" src="https://img.shields.io/badge/PowerPC-static%20recompilation-FF9F0A"></a>
  <a href="#supported-game-revision"><img alt="Dragon Ball Z Budokai Tenkaichi 3 PAL RDSPAF" src="https://img.shields.io/badge/BT3%20PAL-RDSPAF-EF4444"></a>
  <a href="#status"><img alt="Status: work in progress" src="https://img.shields.io/badge/status-work%20in%20progress-F59E0B"></a>
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-2EA44F?logo=gnu&amp;logoColor=white"></a>
</p>

A native PC port of **Dragon Ball Z: Budokai Tenkaichi 3** for Wii, built with static recompilation.

MeteorRecomp translates the game's PowerPC executable ahead of time to native code, then runs it with a Wii runtime/HLE layer and the [Aurora](https://github.com/encounter/aurora) graphics backend. There is no PowerPC interpreter or JIT in the normal runtime path.

> [!IMPORTANT]
> MeteorRecomp does **not** contain the game, a disc image, Nintendo keys, extracted assets, the original executable, or generated translated game code. You must provide your own legally obtained copy of the supported PAL version locally.

---

## Status

MeteorRecomp is under active development and is **not release-ready yet**.

The game can execute through an increasing amount of the original retail code, but compatibility is still being brought up. Missing translated targets and incomplete Wii/runtime behavior can still cause crashes, graphical problems, audio problems, input issues, or other differences from real hardware.

The project prioritizes **retail behavior and parity over shortcuts**. Progress should come from translating the correct guest code or implementing the hardware/runtime behavior it expects, rather than forcing states, faking successful callbacks, or bypassing game logic.

## What it does

**Static PowerPC recompilation.**  
The supported Wii executable is translated ahead of time into native C++ and compiled for the host PC.

**Native rendering through Aurora.**  
GX traffic is handled by [Aurora](https://github.com/encounter/aurora), a source-level GameCube/Wii graphics compatibility layer.

**Higher internal resolutions.**  
The runtime supports multiple internal-resolution multipliers beyond the original Wii resolution.

**Dynamic widescreen / aspect handling.**  
MeteorRecomp includes host-side aspect-ratio handling for the title instead of relying on a fixed Wii output size.

**Runtime settings overlay.**  
Press **F10** while the game window has focus to access runtime options such as display settings, internal resolution, FPS display, audio controls, controller options, and other debugging/compatibility settings.

**Experimental battle-frame interpolation.**  
The runtime includes interpolation paths for higher presentation rates, including the 30-to-60 FPS battle case. This work is still experimental and visual artifacts are expected while parity is being improved.

> [!WARNING]
> A feature being present in the runtime does not mean every game path is already validated. Rendering, audio, input, effects, maps, and title-specific Wii behavior are still being tested and corrected.

## Requirements

For development/building:

- Windows 10 or 11, 64-bit is the primary development target today
- .NET 8 SDK
- CMake 3.25 or newer
- Ninja
- LLVM / Clang
- An LLVM-MinGW toolchain is recommended on Windows
- Your own clean, unmodified **PAL `RDSPAF`** copy of Dragon Ball Z: Budokai Tenkaichi 3

No game files should ever be committed to this repository.

## Supported game revision

MeteorRecomp currently targets one exact retail executable:

| Property | Value |
| --- | --- |
| Title | Dragon Ball Z: Budokai Tenkaichi 3 |
| Platform | Wii |
| Region | Europe / PAL |
| Game ID | `RDSPAF` |
| Entry point | `0x8000403C` |
| SDA / r13 | `0x80631320` |
| SDA2 / r2 | `0x806338C0` |
| `main.dol` SHA-256 | `004f206f760447655e5f4ac8ff554290ea573b186afe9c1f9c331f1b5786b16d` |

Other regions or revisions are not currently supported by the supplied Meteor manifest.

## Preparing your game data

Extract your own supported copy locally. The translator expects the DOL at:

```text
local-data/meteor/DATA/sys/main.dol
```

The manifest verifies the executable hash before translation. Keep all extracted game files under local/private paths and out of Git.

> [!NOTE]
> This project will not provide links to copyrighted game files. Dumping and extracting your own copy is your responsibility.

## Building from source

Owning the supported game is still required when compiling MeteorRecomp yourself.

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

Then configure and build the native executable:

```powershell
cmake --preset release -S projects/meteor
cmake --build --preset release -S projects/meteor --parallel 4
```

The configured output directory is:

```text
build/meteor/native/<host>/bin/
```

## Repository layout

- `projects/meteor/` - title manifest, validated function map, CMake configuration, and Meteor-specific native/HLE bindings
- `translator/` - PowerPC static translator and code-generation pipeline
- `runtime/` - host runtime, Wii HLE services, memory, audio, input, storage, scheduling, settings, and platform integration
- `aurora-main/` - graphics backend used by the runtime
- `benchmarks/` - translator benchmark support

Generated translations, build products, logs, diagnostics, extracted game files, and private development data are intentionally excluded from Git.

## FAQ

**Is MeteorRecomp an emulator?**  
Not in the traditional full-system sense. The game's PowerPC code is translated ahead of time and compiled natively. The host runtime implements or bridges the Wii services and hardware-facing behavior the game expects.

**Do you provide the game or a pre-translated executable?**  
No. The repository does not contain Nintendo game data or generated translated game code.

**Which version of the game works?**  
The current manifest targets the clean European/PAL `RDSPAF` executable with the SHA-256 listed above. Other revisions should not be assumed compatible.

**Why can the game still crash?**  
Bring-up is ongoing. A retail code path may still reach a function that has not been translated or a Wii behavior that the runtime does not implement accurately enough yet. Those failures are treated as bugs to diagnose, not conditions to silently skip.

**Does the FPS option change the game's simulation speed?**  
The current higher-FPS work is host-side frame interpolation. It is intended to increase presentation smoothness without changing the guest game's timing, and it remains experimental.

**Will MeteorRecomp bypass game logic just to get farther?**  
That is not the project's goal. Fixes should preserve the behavior of the retail Wii game as closely as practical.

**Is it finished?**  
No. MeteorRecomp is still in active compatibility and parity development.

## Contributing

Contributions that improve correctness, compatibility, portability, diagnostics, or maintainability are welcome. Please read [`CONTRIBUTING.md`](CONTRIBUTING.md) before submitting changes.

Do not submit copyrighted game files, generated translated game code, or fixes that simply bypass guest behavior to hide a problem.

## Credits

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** - static-recompilation/runtime foundation and reference work used throughout this project
- **[Aurora](https://github.com/encounter/aurora)** - GameCube/Wii graphics compatibility layer used by the runtime
- **[Dolphin Emulator](https://github.com/dolphin-emu/dolphin)** - invaluable open-source reference for Wii hardware and software behavior
- Everyone contributing research and tooling to the static-recompilation community

Third-party components and their licenses are documented in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## License

MeteorRecomp is free software distributed under the **GNU General Public License, version 3**. See [`LICENSE`](LICENSE).

This project is not affiliated with, endorsed by, or associated with Nintendo, Bandai Namco, Spike, or the original game's publishers. Dragon Ball Z: Budokai Tenkaichi 3 and all related trademarks and game content belong to their respective owners. No Nintendo or game publisher intellectual property is distributed with this project.