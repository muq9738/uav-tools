# OSGB → 3D Tiles

**Convert tiled OpenSceneGraph photogrammetry into streamable 3D Tiles 1.1 — then inspect the result without leaving the app.**

[English](README.md) · [简体中文](README.zh-CN.md)

[![License: MIT](https://img.shields.io/badge/license-MIT-27845f)](LICENSE)
[![3D Tiles: 1.1](https://img.shields.io/badge/3D%20Tiles-1.1-3565d4)](native/SPEC.md)
[![Cesium: 1.136](https://img.shields.io/badge/Cesium-1.136-6c5ce7)](app/web/vendor/cesium/SOURCE.json)

A native **C++17 converter**, a **Go desktop and HTTP app**, and an **offline Cesium viewer** in one toolkit. The conversion runtime needs no Node.js, cloud service, browser extension, or separate browser process.

## Highlights

### Fidelity first

- Reconstructs the reachable `PagedLOD` hierarchy and external references without dropping the finest available content.
- Preserves vertex positions, UVs, normals, colors, triangle winding, and original JPEG/PNG encoded bytes by default.
- Uses quantization-free Meshopt compression by default. Geometry values are not simplified or quantized.
- Performs SRS projection and local centering in double precision before writing glTF `FLOAT` values.
- Publishes output only after referenced assets, buffer ranges, extension declarations, and bounding volumes pass integrity checks.

### Optimization when you want it

Draco geometry quantization, KTX2 UASTC/ETC1S textures, and reconstructed distant parent levels are **opt-in**. They can be combined freely, while lossy settings and measured errors are recorded separately in the conversion report.

### A preview that feels built in

Completed jobs open directly in the app's existing system WebView. The bundled Cesium 1.136 viewer loads the actual output — not a separate render or approximation — with model-centric navigation, useful camera presets, quality controls, and live tile statistics.

- Left-drag to orbit around the model surface; right- or middle-drag to pan; scroll to zoom toward the viewport center.
- Switch between perspective, top, and front views, or reset the camera with one click.
- Choose **Smooth**, **Balanced**, or **Detailed** instead of tuning an unexplained graphics parameter.
- High-DPI rendering, 4× MSAA, FXAA, 4× anisotropic filtering, and on-demand rendering at up to 30 fps.
- Fully offline Cesium assets; closing the preview releases its WebGL context, workers, and tile cache.

### Built for real jobs

- File-level parallelism and optional memory-budget scheduling.
- Cooperative cancellation, structured progress events, and persistent job history.
- Content-addressed checkpoints for pause and genuine resume across processes.
- A single packaged native binary shared by the CLI and app, so their defaults and output stay aligned.
- Local-only HTTP by default; explicit address and access-token controls for exposed deployments.

## Quick start

### Build the desktop app on macOS

Prerequisites: C++17, CMake, Ninja, Python 3, Go, and the libraries below. The build also downloads pinned PROJ, Draco, and Basis Universal sources as needed.

```bash
brew install openscenegraph libpng libxml2 sqlite ninja
python3 app/scripts/build.py --sdk-prefix "$(brew --prefix)"
open "app/dist/darwin-arm64/OSGB 转 3D Tiles.app"
```

The build script compiles the converter, runs native and Go tests, collects the runtime and third-party licenses, verifies dependency closure, and creates both the `.app` and a DMG under `app/dist/darwin-arm64/`.

### Convert from the command line

For a converter-only development build:

```bash
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native/build -j 4

./native/build/osgb-tiles /path/to/osgb /path/to/new-tiles \
  --threads 4 \
  --geometry meshopt \
  --texture-format original
```

`INPUT` can be an OSGB directory or an index `.osgb` file. Keep referenced sibling tiles beside the index. The output directory must be new or empty.

When `metadata.xml` does not provide a usable spatial reference, specify it explicitly:

```bash
./native/build/osgb-tiles /path/to/root.osgb /path/to/new-tiles \
  --srs 'EPSG:32651' \
  --origin '224107.81419846753,3366639.462932359,-66.472'
```

ENU is also supported: `--srs 'ENU:latitude,longitude[,height]'`.

### Run the app as a local web service

```bash
./osgb-tiles-app --headless --addr 127.0.0.1:8990
```

Open the printed local URL to create jobs, follow progress, inspect logs, pause/resume supported conversions, and preview completed output. For non-loopback listening, also set `--token` and `--data-dir`.

## Output

```text
new-tiles/
├── tileset.json             # 3D Tiles 1.1 entry point
├── content/*.glb            # Geometry and material payloads
├── textures/*               # Original images or KTX2 output
├── tiles/*.json             # Per-source diagnostic manifests
└── conversion-report.json   # Source mapping, encoding, errors, and statistics
```

Load `tileset.json` in a compatible 3D Tiles client. Meshopt output requires `EXT_meshopt_compression` support; Draco and KTX2 variants require their corresponding decoders. The bundled viewer supports all three.

## Viewer controls

| Input | Action |
| --- | --- |
| Left-drag | Orbit around the picked model-surface center |
| Right-drag / middle-drag | Pan |
| Mouse wheel / pinch | Zoom |
| **Perspective / Top / Front** | Switch camera preset |
| **H** or fit button | Reset to the perspective overview |
| **2** | Top view |
| **F** | Toggle fullscreen |
| **Esc** | Close the embedded preview |
| Smooth / Balanced / Detailed | Adjust streaming detail and screen-space error |

## Conversion options

| Option | Default | Behavior |
| --- | --- | --- |
| `--threads N` | `2` | File-level concurrency, from 1 to 64 |
| `--geometry raw\|meshopt\|draco` | `meshopt` | Raw comparison mode, quantization-free Meshopt, or quantized Draco |
| `--texture-format original\|uastc\|etc1s` | `original` | Original encoded images or KTX2 GPU textures |
| `--rebuild-top` | Off | Adds simplified distant ancestors while preserving original subtrees |
| `--memory-budget MiB` | `0` | Gates submission of later root blocks; not a hard per-file peak limit |
| `--srs` / `--origin` | From metadata | Spatial reference and source-coordinate origin |
| `--checkpoint-dir` / `--pause-file` / `--resume` | Off | File-boundary pause and verified resume |
| `--json-events` / `--cancel-file` | Off | Structured progress and cooperative cancellation |
| `--capabilities` | — | Prints the machine-readable native capability contract |

Lossy features are never presented as lossless. UASTC improves GPU residency and decoding behavior but usually produces much larger files than JPEG. Rebuilt-parent texture baking is also lossy; its sampling offset is reported and is not an upper bound for whole-surface error.

## Project layout

```text
native/   C++17 converter, CLI, specifications, and native tests
app/      Go/MyGo desktop and HTTP app, embedded Cesium viewer, packaging
data/     Small synthetic OSGB fixtures
```

The app invokes the packaged `osgb-tiles` binary directly. There is one converter implementation and one runtime, shared by desktop, portable, and CLI distribution.

## Tests and verification

```bash
# Native behavior and regression tests
ctest --test-dir native/build --output-on-failure

# App, API, job management, and preview resource isolation
cd app && go test ./...

# Distribution metadata tests
python3 app/tests/test_distribution.py

# Verify a staged package and its resource hashes
python3 app/scripts/build.py --verify-package app/dist/darwin-arm64
```

The test suite covers paging and references, geometry and coordinate handling, texture compatibility, encoding round trips, publication integrity, pause/resume, HTTP job management, and preview path containment. Independent decoders are used for critical geometry checks rather than validating the encoder only with itself.

## Platform status

- **macOS arm64:** built and validated with the packaged desktop app, CLI, runtime, and embedded Cesium preview.
- **Linux and Windows:** build definitions are provided in `.github/workflows/native-app.yml`, but full native, model, and desktop validation has not been completed.
- Windows requires WebView2; Linux desktop use requires the applicable WebKit and desktop services. Portable HTTP mode has a smaller platform surface.

## Documentation

- [Converter behavior and build details](native/README.md)
- [Native behavior specification](native/SPEC.md)
- [App, packaging, and distribution](app/README.md)
- [HTTP API](app/docs/HTTP-API.md)
- [Pause and resume contract](app/docs/PAUSE-RESUME.md)

## License

Released under the [MIT License](LICENSE). OpenSceneGraph, PROJ, Cesium, Draco, Basis Universal, and other third-party components retain their own licenses; the corresponding notices and source information are included in packaged distributions.
