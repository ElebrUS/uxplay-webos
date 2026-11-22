# Changelog

## [0.3.7]

- Retry empty or prematurely ended Safari streams and distinguish them from normal completion.
- Fall back to the original HLS source when a profiled MP4 produces no video.

## [0.3.6]

- Resume Safari playback from a short buffer after transient network stalls.
- Retry interrupted reads on seekable clips, up to twice per clip.
- Reduce expensive uploads and discard stale audio when direct playback falls behind.
- Use resolution and frame rate to choose a sustainable HLS variant.

## [0.3.5]

- Include the webOS startup and connection recovery patch in reproducible builds.
- Decode direct Safari video using frame threads and skip redundant clears for full-screen video.
- Preserve presentation timestamps during seeks and drain delayed video frames at end of stream.

## [0.3.4]

- Accept direct HTTP(S) MP4 and unencrypted HLS playback without a per-site profile.

## [0.3.3]

- Preview seek targets before committing, accelerate held arrows, and reuse the MP4 index.

## [0.3.2]

- Redesign the receiver, pairing screen and Safari playback interface.

## [0.3.1]

- Add remote playback controls and a seek bar for direct Safari video.

## [0.3.0]

- Advertise one receiver for screen mirroring and direct Safari playback.

## [0.2.0]

- Add the ACB-free H.264/SDL receiver build for webOS 1.4 qualification.
- Audit the ARM ELF against glibc 2.14.1 and package the private C++ runtime.

All notable changes to this project are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-09-07

### Added

- Experimental native AirPlay receiver for rooted webOS TVs.
- H.264 mirroring, stereo PCM/AAC/ALAC audio handling, PIN pairing, AirPlay discovery, and adjustable audio delay.
- Reproducible local build, ABI verification, source-package generation, sanitizer-backed host tests, and release documentation.

### Known limitations

- Validated only on the LG 32LJ600B-SA with firmware 06.10.60.
- DRM/HLS, HEVC/HDR, recording, startup at boot, and broad-device compatibility are unsupported or unverified.
