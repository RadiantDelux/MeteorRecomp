# Contributing to MeteorRecomp

Contributions are welcome when they improve correctness, compatibility, portability, diagnostics, or maintainability.

## Guidelines

- Keep changes focused and explain the technical reason for them.
- Preserve original guest-visible behavior. Avoid fixes that only skip waits, force state, fake callbacks, or bypass game logic.
- Keep title-specific addresses and policies under `projects/meteor/`.
- Put reusable Wii/runtime behavior in the shared runtime when it is not specific to this title.
- Do not edit generated translation output as a permanent solution.
- Add or update tests when changing reusable translator or runtime behavior.
- Include enough reproduction or validation detail for behavior changes to be reviewed.

## Game data

Never submit Nintendo code, disc images, extracted game assets, keys, saves containing personal data, or generated translated game code.

Development should use a legally obtained local dump that remains outside Git.

## Pull requests

Describe what changed, why it changed, and how it was validated. For changes that affect game behavior, include the relevant runtime evidence or comparison used to establish correctness.

## License

By contributing, you agree that your contribution is distributed under the repository's GNU GPL v3 license.
