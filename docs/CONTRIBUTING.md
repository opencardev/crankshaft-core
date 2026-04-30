# Contributing to crankshaft-core

## Scope

This guide covers contribution workflow, coding standards, and PR expectations for `crankshaft-core`.

## Getting Started

1. Fork or branch from `main`.
2. Install dependencies:

```bash
./build.sh --install-deps
```

3. Build and run tests:

```bash
./build.sh --clean
```

## Development Workflow

1. Create a focused branch for one logical change.
2. Keep commits small and descriptive.
3. Re-run quality and tests before opening a PR.

Recommended pre-PR checks:

```bash
CODE_QUALITY=ON FORMAT_CHECK=ON BUILD_TESTS=ON ./build.sh --clean
```

## Coding Standards

- Preserve existing architecture and naming patterns.
- Prefer minimal, targeted changes over broad refactors.
- Keep public behavior stable unless explicitly changing an API.
- Add or update tests when behavior changes.

## Pull Request Expectations

Each PR should include:

- Problem statement (what is broken or missing)
- Solution summary (what changed)
- Risk notes (what areas may be impacted)
- Test evidence (commands run and outcome)

## Commit Guidance

- Use clear commit titles in imperative mood.
- Avoid mixing unrelated fixes in one commit.

Examples:

- `Fix session teardown race during reconnect`
- `Add SBOM-only mode to build script`

## Reporting Issues

When filing an issue, include:

- Environment (OS, arch, compiler/toolchain)
- Exact steps to reproduce
- Expected vs actual behavior
- Relevant logs or stack traces

## Community Conduct

All contributors are expected to follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
