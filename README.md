# RtAudioLinux's SuperLooper

SuperLooper is a Linux JACK-based intelligent looper built with Qt Widgets. It is designed for live sample recording, assignment, trimming, looping, sync, and mixer control from a computer keyboard, mouse, on-screen pedals, and optional MIDI input.

The application registers as a JACK client named `SuperLooper`.

## Current Status

This project is under active development. The primary tested build path is Qt5. CMake includes Qt6 selection support, but the Qt6 build path is currently untested.

## Features

- 88-key piano widget for mouse, QWERTY, and MIDI note input.
- Mode cycle: Normal -> Record -> Playback -> Edit.
- JACK stereo capture and playback ports.
- Drag audio files into the sample pool, then assign samples to piano keys.
- Record fixed-length, held, or auto-trimmed loops.
- Root loop tempo calculation and RubberBand sync stretching.
- Sample pool A/B labeling, layered playback, append, trim, clone, export, and delete.
- Edit-mode key controls: volume, pan, mute, solo, self-mix, group bus, and play/pause.
- Master gain, peak meter, final soft limiter, fades, and optional loop crossfade.
- Four mixer group buses: Group A, Group B, Group C, Group D.
- Runtime resampler selection with automatic preference for soxr when available.
- JSON state save/load, including samples, key assignments, trims, mixer settings, and app settings.
- Expanded SFZ import/export support, including inherited defaults, key ranges, trim offsets, pan, volume/gain, attack/release, loop mode, loop points, and pitch-center / pitch-keytrack / transpose / tune mapping.
- Export Current Looper State as SFZ Bundle writes an `.sfz` plus sample folder and preserves additional SuperLooper key/global metadata in comments and custom opcodes such as `x_superlooper_selfmix`.
- Support for individual key loop-off and adjustable virtual staccato / articulation settings.
- In `No Loop` mode, virtual staccato is release-driven: held notes play normally until key-up, then ring briefly and fade unless disabled for that key.
- Optional LV2 hosting through `lilv`, including 5-slot per-key FX chains and a 5-slot master FX chain.

## Dependencies

Install the development packages for:

- CMake 3.20+
- Ninja or another CMake generator
- C++20 compiler
- Qt5 Widgets, or Qt6 Widgets if testing Qt6
- JACK
- libsndfile
- FFmpeg libraries: `libavformat`, `libavcodec`, `libavutil`, `libswresample`
- RubberBand
- RtMidi
- Aubio
- Optional: soxr
- Optional: libsamplerate
- Optional: lilv / LV2
- Optional: sfizz (planned for later as an integrated SFZ loader as an alternative to our in-house solution which evolved from the key-based sampler
)

On Debian/Ubuntu-style systems, the package set is typically similar to:

sudo apt install build-essential cmake ninja-build \
  qtbase5-dev qttools5-dev-tools \
  libjack-jackd2-dev libsndfile1-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
  librubberband-dev librtmidi-dev \
  libsoxr-dev libsamplerate0-dev \
  lilv-utils liblilv-dev libaubio-dev

Package names vary by distribution.

## Build

From the project root:

cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure

Run:

./build/SuperLooper

## Qt Selection

CMake supports selecting the Qt major version:

cmake -S . -B build -G Ninja -DSUPERLOOPER_QT_VERSION=AUTO
cmake -S . -B build -G Ninja -DSUPERLOOPER_QT_VERSION=5
cmake -S . -B build -G Ninja -DSUPERLOOPER_QT_VERSION=6

`AUTO` prefers Qt5 when available. If Qt5 is not available and Qt6 is available, it tries Qt6. Qt6 support is present but untested.

## Optional Resamplers

SuperLooper always has FFmpeg swresample available through the FFmpeg dependency. Additional resamplers can be controlled with:

cmake -S . -B build -G Ninja -DSUPERLOOPER_WITH_SOXR=ON
cmake -S . -B build -G Ninja -DSUPERLOOPER_WITH_LIBSAMPLERATE=ON

When built in, soxr is the preferred automatic backend. Runtime selection is available from `Settings -> Audio and Mixer Settings...`.

Optional LV2 support can be controlled with:

cmake -S . -B build -G Ninja -DSUPERLOOPER_WITH_LILV=ON

When built in, stereo LV2 plugins can be attached in up to five chained slots per piano key and up to five chained slots on the master output, with per-slot enable toggles and parameter dialogs.

## JACK Setup

Start JACK before connecting the app. Then use:

Audio -> Connect to JACK

SuperLooper creates two input ports and two output ports, and it tries to auto-connect physical capture and playback ports.

## Basic Workflow

1. Start JACK.
2. Launch `SuperLooper`.
3. Choose `Audio -> Connect to JACK`.
4. Drag audio files from the file browser into the sample pool.
5. Drag a sample from the pool to a piano key to assign it.
6. Use the left pedal or grave key to cycle modes.
7. In Record mode, press a key to record a loop for that key.
8. In Playback mode, press an assigned key to start looping; press again to stop at the loop end.
9. In Edit mode, select a key and adjust volume, pan, mute, solo, group bus, and play/pause.
10. Right-click a piano key to adjust articulation, loop mode, and optional per-key LV2 chain slots.
11. Use `Settings -> Master LV2 Chain...` to manage the five master LV2 chain slots on the full output mix.

## Keyboard Shortcuts

- Grave key: cycle Normal -> Record -> Playback -> Edit.
- `[` / `]`: shift QWERTY piano octave.
- Arrow keys in sample pool: move selected sample.
- Delete: remove/delete selected sample.
- `A` / `B`: label selected sample for A/B operations.
- Space: layered play A+B.
- Enter: append B to A as a new sample.
- Ctrl+T: auto-trim selected sample.
- Ctrl+S: edit start marker.
- Ctrl+E: edit end marker.
- Ctrl+C: create a trimmed clone from start/end markers.
- Shift+E: export in-memory samples to a typed directory.

## State Files

Use:

File -> Save State...
File -> Load State...

State files are JSON. If samples only exist in memory, SuperLooper writes WAV files beside the state file in a sibling sample folder.

## Notes

- The primary tested path remains the native in-house looper renderer.
- SFZ import/export is functional and broader than the initial subset, but it is not yet complete SFZ opcode coverage.
- LV2 support currently focuses on stereo 2-in / 2-out audio effects discovered through `lilv`, arranged in fixed five-slot chains for keys and master.

## License

SuperLooper is licensed under the GNU General Public License version 3. See [COPYING](COPYING).
