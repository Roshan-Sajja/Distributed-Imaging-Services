# Distributed Imaging Services

A small C++ pipeline that streams images over ZeroMQ, extracts SIFT features, and logs results to disk/SQLite. You can run it locally or with Docker.

## Prerequisites (local build)
- C++20 compiler (clang++-17 or g++-12+)
- CMake ≥ 3.26, Ninja/Make
- OpenCV 4, ZeroMQ, SQLite3, pkg-config

Install system deps:
- macOS (Homebrew): `brew install cmake ninja pkg-config opencv zeromq sqlite3`
- Ubuntu/Debian: `sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build pkg-config libopencv-dev libopencv-contrib-dev libzmq3-dev libsqlite3-dev`

## Configure the environment
Copy and edit the env file (used by all apps):
```bash
cp env.example .env
# adjust paths/endpoints if needed
```

Key outputs (defaults):
- Raw frames: `storage/raw_frames`
- Annotated frames: `storage/annotated_frames`
- SQLite DB: `storage/dist_imaging.sqlite`

## Build and run (local)
```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/bin/data_logger --env .env
./build/bin/feature_extractor --env .env   # new terminal
./build/bin/image_generator --env .env     # new terminal (add --once to send one pass)
```

Helper scripts (from repo root):
```bash
./scripts/bootstrap.sh   # configure
./scripts/build.sh       # build
./scripts/run_all.sh     # run logger + extractor + generator together
```

Common flags:
- `--once` (image_generator / run_all): stream the dataset a single time, then exit.
- `--annotated` (feature_extractor / run_all): emit annotated frames; the logger will write them to `storage/annotated_frames`.

## Docker
We bake the code and .env into the image at `/app`.

Build and run:
```bash
docker compose build --no-cache dist-imaging
docker compose up dist-imaging
```

Keep outputs on the host (already set in compose):
- Host `./storage` is mounted to `/app/storage` in the container.

Run the pushed image elsewhere (example):
```bash
mkdir -p storage
docker run --rm \
  -v "$PWD/storage":/app/storage \
  roshansajja/dist-imaging:1.0.0
```
Mount a different env to override the baked one:
```bash
docker run --rm \
  -v "$PWD/storage":/app/storage \
  -v "$PWD/.env":/app/.env:ro \
  roshansajja/dist-imaging:1.0.0
```

## Notes
- `DIST_ENV_PATH` can override the `.env` location.
- Use `--log-level` on any binary to override log verbosity.
- If you add new dependencies, install them in both your host and the Dockerfile.
