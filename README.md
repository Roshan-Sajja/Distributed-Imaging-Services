# Distributed Imaging Services (Skeleton)

Minimal placeholder C++ project ready to be pushed to GitHub.

## Build and run (CMake)

This repository now uses a multi-target CMake build. The only prerequisites are a
C++20 compiler (clang++-17 or g++-12+), CMake ≥3.26, and Ninja/Make.

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

Executables are emitted into `build/bin/`:

```bash
./build/bin/image_generator --help   # placeholder stub
./build/bin/feature_extractor
./build/bin/data_logger
```

Enable `-DDIST_WARNINGS_AS_ERRORS=ON` during configuration if you want CI-style
strictness.

## Environment configuration

Runtime settings for all three services are injected through a `.env` file
following the variables defined in `env.example`. To get started:

```bash
cp env.example .env
mkdir -p data/images storage/raw_frames
```

| Variable | Purpose |
| --- | --- |
| `APP_LOG_LEVEL` | Controls shared logging verbosity (`trace` → `critical`). |
| `METRICS_HTTP_PORT` | Optional HTTP port for process metrics. |
| `IMAGE_GENERATOR_*` | Paths, publish endpoint, loop cadence, and heartbeat for App 1. |
| `FEATURE_EXTRACTOR_*` | Subscriber/publisher endpoints plus SIFT tuning knobs. |
| `DATA_LOGGER_*` | Subscriber endpoint and persistence locations for App 3. |

Update the copied `.env` with paths/endpoints that match your deployment (local
TCP sockets, ZeroMQ `ipc://` endpoints, etc.). The actual `.env` is gitignored
to keep machine-specific values out of version control.

Each executable automatically loads `.env` from the working directory (you can
override by setting the `DIST_ENV_PATH` environment variable). If the file is
missing or malformed, the process exits with a clear error so misconfigured
deployments fail fast.

## Next steps

- Replace the dummy program with real functionality.
- Add tests, a build system (CMake/Make), and CI once project requirements are known.
