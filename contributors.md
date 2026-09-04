# Contributing to sendspin-cli

Thanks for contributing. Please open an issue or pull request in the
[repository](https://github.com/Sendspin/sendspin-cpp-cli). For user installation,
configuration, and troubleshooting documentation, update the source files in
[`docs/wiki/`](docs/wiki); the wiki is generated from those files on pushes to `main`.

## Development setup

You need CMake 3.16 or later, a C++20 compiler, and network access for the first
configure. CMake fetches the pinned `sendspin-cpp` dependency and its dependencies.

Install the audio and mDNS development packages for the backends you want to exercise:

```bash
sudo apt install pkg-config libasound2-dev portaudio19-dev libpulse-dev libpipewire-0.3-dev libavahi-compat-libdnssd-dev
sudo dnf install pkgconf alsa-lib-devel portaudio-devel pulseaudio-libs-devel pipewire-devel avahi-compat-libdnssd-devel
brew install portaudio pkgconf
```

Then configure, build, and test:

```bash
cmake -B build -DSENDSPIN_CLI_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Audio backends and mDNS are optional and auto-detected. Use
`-DSENDSPIN_CLI_WITH_ALSA=OFF`, `-DSENDSPIN_CLI_WITH_PORTAUDIO=OFF`,
`-DSENDSPIN_CLI_WITH_PULSE=OFF`, `-DSENDSPIN_CLI_WITH_PIPEWIRE=OFF`, or
`-DSENDSPIN_CLI_WITH_MDNS=OFF` to test a reduced build. Use
`-DSENDSPIN_CLI_BUILD_TESTS=OFF` for a quick build without the test suite.

## Before opening a pull request

- Keep changes focused and add or update tests for behavior changes.
- Run the focused CTest tests, or the complete `ctest --test-dir build --output-on-failure`
  suite when the change crosses components.
- Run `shellcheck scripts/*.sh` after changing a shell script.
- Update the appropriate generated-wiki source in `docs/wiki/` when user-facing
  behavior changes.

GitHub Actions builds and tests Linux, macOS, and ARM targets. The ARMv6 build is
separate because it runs under emulation; it is required for releases.

## Project layout

- `src/` contains the player, command-line interface, output backends, discovery, and
  local control implementation.
- `tests/` contains the GoogleTest unit suite.
- `packaging/` contains the service, sysusers, and configuration-file payload.
- `scripts/` contains installer, packaging, and smoke-test scripts.
- `docs/wiki/` contains the source for the published wiki.

## Releases

Release tags use the exact form `vMAJOR.MINOR.PATCH` and must match the version in
`CMakeLists.txt`. Before pushing a release tag, add
`docs/release-notes/<version>.md` using the existing release notes as a template.

Pushing the tag runs the release workflow, builds the platform archives and macOS
installer, verifies the expected assets, and publishes the GitHub Release only when all
required builds succeed.
