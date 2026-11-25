---
name: project-build
description: Build, test, package, or release this repository's Windows x64 and macOS ARM64 applications. Use when preparing a version, running CI, signing updates, or publishing a GitHub release.
---

# Project builds and releases

| Platform | Build environment | Verification |
| --- | --- | --- |
| Windows x64 | GitHub Actions on `windows-2022` | Native and Go tests, packaged App/CLI parity, installer and installed application |
| macOS ARM64 | Local Apple Silicon Mac | Native and Go tests, packaged App/CLI parity, DMG and signed update archive |
| Linux | Paused | Skip Linux builds and release artifacts until requested |

Use English for commit messages, annotated tags, workflow names, and release notes. Run the commands below from the repository root unless a command changes directories.

## Windows

[Windows Build and Release](../../../.github/workflows/release.yml) runs on pushes to `main`, pull requests and version tags. A manual run on a branch builds the Windows package. A run on a `v*` tag also signs updates and stages verified assets in a draft release. Reuse an active run for the release tag. Retry a failed Windows job after checking its logs.

## Local macOS build

1. Check out the release commit on an Apple Silicon Mac. Install the SDK listed in the [build instructions](../../../app/README.md). For GeoTIFF changes, make `gdalinfo`, `gdallocationinfo`, and `gdalsrsinfo` available to CMake and confirm that `native-orthophoto-gdal` runs.
2. Build the native runtime and portable application, then verify the package and exercise App/CLI parity with the generated textured OSGB fixture.

   ```bash
   python3 -m unittest discover -s app/tests -p 'test_*.py'
   python3 app/scripts/build.py --sdk-prefix "$(brew --prefix)" --skip-desktop
   python3 app/scripts/build.py --verify-package app/dist/darwin-arm64/portable
   input=$(find app/build/darwin-arm64 -name 'texture+alpha.osgb' -print -quit)
   python3 app/tests/verify_parity.py --package app/dist/darwin-arm64/portable \
     --input "$input" --srs ENU:31.2,121.4,10 --evidence .agents-debug/mac-parity
   ```

3. Supply `MYGO_UPDATER_PRIVATE_KEY` from the local signing-key store in the build process environment. MyGo checks it against `updates.publicKey` in `app/mygo.json`. Keep publishing credentials out of application linker flags. Build the desktop app with `(cd app && go tool mygo build --platform darwin/arm64)`.
4. Mount the generated DMG read-only and extract the update tarball into a separate temporary directory. Run `python3 app/scripts/build.py --verify-package` on each enclosed `.app`. Check the bundle version and verify the archive against `update-darwin-arm64.json` and the configured public key. Detach the disk image after verification.

## Publish a version

1. Set the version in `app/mygo.json` and the Go entry point, and add the English release notes to `app/CHANGELOG.md`. Test the release commit and push its annotated `v<version>` tag.
2. Wait for the Windows tag job to stage its installer, update archive, manifest, and available deltas. Upload the verified local macOS DMG, update archive, manifest, and available deltas for the same version to that draft with `gh release upload`.
3. Inspect the draft asset list. Confirm that both platforms have their installation package, update archive, and update manifest. Run `gh release edit "$tag" --draft=false --latest`, then verify the public release URL, asset list, and that the remote tag resolves to the tested commit.
