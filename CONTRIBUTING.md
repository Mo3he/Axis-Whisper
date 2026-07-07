# Contributing

Thanks for helping improve this ACAP. Bug reports, fixes, features, and docs are
all welcome.

## Reporting issues

- Bugs and feature requests: open a GitHub issue and include your camera model,
  Axis OS version, the app version (or `.eap` filename), and steps to reproduce.
- Security vulnerabilities: please do not open a public issue; follow
  `SECURITY.md`.

## Building

This app is **aarch64-only** (whisper.cpp needs the 64-bit SoCs) and builds from
the `aarch64/` directory using the Axis ACAP Native SDK image. Docker (or Apple
`container`) must be installed:

```sh
cd aarch64
docker build --tag axis-whisper .
docker cp "$(docker create axis-whisper)":/opt/app ./build   # extract the .eap
```

Install the resulting `.eap` on a camera under **Apps > Add app** in the device
web interface, then check the app log under **Apps > (this app) > App log** to
confirm inference starts.

## Pull requests

1. Fork the repository and branch from `main`.
2. Keep each pull request focused on one logical change.
3. Build locally, and where possible install and smoke-test the `.eap` on a
   device before submitting.
4. Update `README.md` when behaviour or settings change.
5. Explain what the change does and why in the description.

## Code style

- If you change C code, format it with `clang-format` using the `.clang-format`
  in this repository.
- Keep Markdown lint-clean (`.markdownlint.yaml`): wrap bare URLs and emails in
  angle brackets and give code fences a language.
- Match the surrounding code and keep diffs minimal.

## Licensing

By contributing, you agree that your contributions are licensed under this
repository's `LICENSE`. Bundled components (whisper.cpp / ggml) remain under
their own licenses (see `THIRD_PARTY_NOTICES.md`). This is an independent,
community project and is not affiliated with or endorsed by Axis Communications.
