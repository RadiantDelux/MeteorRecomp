# Meteor project

This directory contains the title-specific configuration for **Dragon Ball Z: Budokai Tenkaichi 3 (Europe), RDSPAF**.

- `recomp.yml` defines the supported DOL identity, memory layout, translation roots, function map, and generated-output paths.
- `function_map_ghidra_safe.txt` contains validated function-boundary seeds used by the translator.
- `runtime_native/` contains Meteor-specific native/HLE bindings.
- `CMakePresets.json` configures the native Meteor build.

Private extracted inputs belong under `local-data/meteor/` at the repository root and generated code belongs under `build/meteor/`. Neither directory is tracked by Git.

See the repository [README](../../README.md) for the supported revision and build procedure.
