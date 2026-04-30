# crankshaft-core Documentation

## Overview

`crankshaft-core` is the backend runtime for the Crankshaft platform. It manages Android Auto session lifecycle, transports, hardware abstraction layers, media, Bluetooth, configuration, and service coordination.

Primary source tree: [src](../src)

## Repository Layout

- [src](../src): core runtime sources and tests
- [deps](../deps): OS package manifests consumed by [build.sh](../build.sh)
- [build.sh](../build.sh): canonical entry point for local and CI builds
- [.github/workflows/build.yml](../.github/workflows/build.yml): CI pipeline

## Build and Test

Typical local build:

```bash
./build.sh --clean
```

Run tests:

```bash
BUILD_TESTS=ON ./build.sh --clean
```

Install host dependencies via build script:

```bash
./build.sh --install-deps
```

Quality checks:

```bash
CODE_QUALITY=ON FORMAT_CHECK=ON ./build.sh --clean
```

Coverage build:

```bash
ENABLE_COVERAGE=ON BUILD_TESTS=ON ./build.sh --clean
```

Package build:

```bash
BUILD_PACKAGE=ON ./build.sh --clean
```

SBOM generation (requires package output):

```bash
BUILD_PACKAGE=ON BUILD_SBOM=ON ./build.sh --clean
```

Generate SBOM only from an existing build directory:

```bash
BUILD_DIR=build-release BUILD_SBOM=ON ./build.sh --sbom-only
```

## AASDK Notes

By default, the build script requires AASDK to be available (`WITH_AASDK=1`) via installed packages (`libaasdk` and `libaasdk-dev`).

Install dependencies before build:

```bash
./build.sh --install-deps
```

During dependency installation, `build.sh` configures the OpenCarDev apt repository (`https://apt.opencardev.org`) for the current distro codename and architecture, then installs required packages.

Optional controls:

- Disable AASDK requirement check: `WITH_AASDK=0`

## CI Summary

The CI pipeline includes:

- Unified quality + coverage + quick packaging gate
- Full multi-arch build and test matrix
- Build artifact uploads
- Source and package SBOM generation per build
- Release metadata generation (changelog + release notes)

## Project Policies

- Contribution process: [CONTRIBUTING.md](CONTRIBUTING.md)
- Community standards: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
