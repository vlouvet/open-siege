# Linux AppImage packaging

Produces `Open-Siege-x86_64.AppImage` — a single-file portable Linux
executable that runs on any glibc ≥ 2.35 distro without installation.

## Prerequisites

1. A prebuilt `dts-viewer` binary (see `../../BUILD-LINUX.md`)
2. Internet access (the script auto-downloads `linuxdeploy` + `appimagetool`
   on first run; subsequent runs reuse the cached binaries)
3. `libfuse2` installed on the build host (or use `--appimage-extract-and-run`
   if FUSE is unavailable — e.g., inside a Docker container)

```sh
sudo apt-get install libfuse2
```

## Building the AppImage

```sh
cd packaging/linux
bash build-appimage.sh
```

This produces `../../Open-Siege-x86_64.AppImage` (≤ 150 MB target).

### Environment variables

| Variable  | Default                                      | Purpose           |
|-----------|----------------------------------------------|-------------------|
| `BINARY`  | `../../examples/dts-viewer/build/dts-viewer` | Path to the binary|
| `OUTPUT`  | `../../Open-Siege-x86_64.AppImage`           | Output path       |

Example with custom binary:
```sh
BINARY=/path/to/dts-viewer bash build-appimage.sh
```

## Running the AppImage

```sh
chmod +x Open-Siege-x86_64.AppImage
./Open-Siege-x86_64.AppImage
```

First launch will prompt for your Tribes 1.41 install directory.
The path is cached in `~/.config/open-siege/config.toml`.

### Without FUSE (Docker, CI containers)

```sh
./Open-Siege-x86_64.AppImage --appimage-extract-and-run
```

## Releases via GitHub Actions (CI)

See `.github/workflows/build-linux.yml` (spec 23/05).

Trigger a release by pushing a version tag:
```sh
git tag v0.1.0
git push origin v0.1.0
```

The CI workflow builds the AppImage on `ubuntu-24.04` runners and uploads
it as a GitHub Release asset automatically.

For branch builds (no tag), the AppImage is available as a workflow
artifact in the GitHub Actions UI for 90 days.

## License compliance

The AppImage carries two MIT licenses:
- Open Siege (`../../LICENSE`)
- Torque 3D console VM (`../../3space/cscript/LICENSE` or equivalent)

Both are bundled in `AppDir/usr/share/doc/open-siege/LICENSE` by the
build script. The AppImage does **not** contain any Tribes 1.41 game data.

## Files in this directory

| File                      | Purpose                                |
|---------------------------|----------------------------------------|
| `build-appimage.sh`       | Main packaging script                  |
| `open-siege.desktop`      | `.desktop` entry for AppImage metadata |
| `open-siege.png`          | 256×256 icon (placeholder for v1)      |
| `conan-profile-linux`     | Conan v1 profile for gcc 15 + x86_64  |
| `build/`                  | Scratch dir (gitignored)               |
