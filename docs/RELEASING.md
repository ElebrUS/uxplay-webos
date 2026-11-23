# Releasing UxPlay webOS Legacy

The fork is developed on `webos-1.4-port`. GitHub Actions validates changes and builds an ARMv7 IPK and its corresponding source archive on Apple Silicon. Branch pushes and pull requests produce an Actions artifact. Releases require an explicit version tag pushed to [ElebrUS/uxplay-webos](https://github.com/ElebrUS/uxplay-webos).

## Prepare a release

1. Set the version in `packaging/appinfo.json` and add the matching section to `CHANGELOG.md`.
2. Run `python3 tools/validate-metadata.py`, `tools/test.sh`, and `tools/test-software.sh`.
3. On macOS ARM64, run `tools/build.sh` and `python3 tools/source-package.py`. The build runs ELF compatibility checks before packaging.
4. Verify the IPK on compatible hardware when available. Record which firmware and playback modes were exercised and document any gaps.
5. Push the release commit to the fork and check the branch workflow.

## Publish

Create and push an annotated tag in the form `v<version>` for the release commit. The tag must match the package version exactly. After the host tests and ARM build succeed, GitHub Actions creates the release from the matching changelog section and attaches the IPK, corresponding source archive, and `SHA256SUMS`.

This workflow publishes GitHub Releases. It does not submit a webOS Brew catalog entry or install the package on a TV.
