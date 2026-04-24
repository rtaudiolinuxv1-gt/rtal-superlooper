# SuperLooper Testing Checklist

## Core Audio

- [ ] Connect to JACK and confirm auto-connect to physical capture/playback ports
- [ ] Record a fixed-length loop
- [ ] Record a held loop
- [ ] Record an auto-trimmed loop
- [ ] Playback mode start/stop works for assigned keys
- [ ] Emergency Stop silences everything immediately
- [ ] Peak meter and limiter respond under stacked loud playback

## Sample Pool / Editing

- [ ] Drag files from the file browser into the sample pool
- [ ] Drag sample-to-key assignment works with mouse
- [ ] Arrow-key sample navigation works
- [ ] Delete behavior is correct for imported / empty / app-created samples
- [ ] Ctrl+S / Ctrl+E trim search still behaves correctly
- [ ] Ctrl+C creates a trimmed clone correctly
- [ ] Shift+E exports in-memory samples
- [ ] Right-click sample export works
- [ ] Right-click sample label works

## Sync / SFZ

- [ ] Root tempo calculation updates correctly
- [ ] Sync-to-root stretching works
- [ ] SFZ import loads a representative simple instrument
- [ ] SFZ import respects inherited defaults and key ranges
- [ ] SFZ import respects offset/end/pan/volume/tuning mapping
- [ ] SFZ import respects `gain`, `loopmode`, `loop_start` / `loop_end`, and `pitch_keytrack`
- [ ] SFZ export writes a usable SFZ plus sample folder
- [ ] SFZ export writes `loopmode` alias plus loop-point opcodes for looping keys
- [ ] RubberBand import resampling handles long files without truncating near 10 seconds
- [ ] Root-sync RubberBand stretching handles long samples without truncation or silence tails
- [ ] Real-world check: import a long mismatched-rate song with RubberBand selected and confirm full duration, correct pitch, and normal waveform shape
- [ ] Real-world check: stretch a long recorded or imported sample to root duration and confirm full-length output with no cutoff near 10 seconds

## Mixer / Key Editing

- [ ] Edit mode key selection works
- [ ] Per-key volume / pan / mute / solo / group work
- [ ] Per-key articulation settings work
- [ ] No Loop mode works
- [ ] No Loop + Mix With Itself allows repeated overlapping retriggers of the same key
- [ ] Held `No Loop` notes keep playing normally until key release
- [ ] `Virtual Staccato` on a `No Loop` key starts only after key release, adds a short tail, then fades
- [ ] `Virtual Staccato` defaults on for `No Loop` keys and can be turned off per key
- [ ] Quick taps on `No Loop` keys produce short notes while held notes sustain until release
- [ ] Turning `Virtual Staccato` off on a `No Loop` key stops release-shortening and lets the one-shot continue naturally
- [ ] Edit mode shows the selected key Self Mix state and changing it updates playback behavior
- [ ] Group bus controls work
- [ ] Master gain works

## LV2

- [ ] LV2 discovery lists usable stereo plugins
- [ ] Per-key LV2 attachment works
- [ ] Master LV2 attachment works
- [ ] Per-key LV2 parameter editor updates plugin controls audibly
- [ ] Master LV2 parameter editor updates plugin controls audibly
- [ ] Per-key LV2 chain supports 5 slots with distinct plugin selections
- [ ] Master LV2 chain supports 5 slots with distinct plugin selections
- [ ] Per-key LV2 slot enable/disable toggles bypass processing without removing the plugin assignment
- [ ] Master LV2 slot enable/disable toggles bypass processing without removing the plugin assignment
- [ ] Parameter dialogs open and work for all 5 per-key LV2 slots
- [ ] Parameter dialogs open and work for all 5 master LV2 slots
- [ ] Plugins which require additional features are identified clearly
- [ ] Save/load restores LV2 attachments
- [ ] Save/load restores LV2 parameter values
- [ ] Replacing or clearing LV2 plugins repeatedly while JACK is running does not crash or leak obvious memory

## SFZ Bundle Export

- [ ] Export Current Looper State as SFZ Bundle writes a bundle successfully
- [ ] Exported SFZ bundle uses valid sample paths relative to `default_path`
- [ ] Exported SFZ bundle captures current global looper settings in metadata comments
- [ ] Exported SFZ bundle captures per-key mixer and articulation metadata
- [ ] `x_superlooper_selfmix=1` is exported for self-mix keys and imported back correctly

## Root Timing / Beat Detection

- [ ] Setting a rhythmically clean loop as `Root` shows a beat-aware timing analysis message rather than only raw duration sync
- [ ] After setting `Root`, the top BPM display stays on `BPM: Processing` until aubio analysis completes and never flashes a guessed duration-based BPM
- [ ] Beat-aware root analysis trims leading silence/off-grid attack from the root playback window when the root key still uses default trims
- [ ] Changing `Bars` after root analysis re-snaps the root playback window and updates the global tempo accordingly
- [ ] `syncToRoot` samples stretch to the analyzed root bar length rather than the raw file length
- [ ] Manually trimmed root keys are not overwritten by automatic beat-aware root trim updates
- [ ] Enabling `Sync` on a non-root sample shows `BPM: Processing` until that sample's beat analysis completes
- [ ] After `Sync` is enabled and analysis is ready, the selected sample status changes to `BPM: Syncing BPM` until the stretch finishes
- [ ] After sync completes, the selected sample BPM display shows `source BPM > root BPM`
- [ ] Samples with no stable detected beat grid fail cleanly instead of stretching from a guessed BPM
- [ ] Sync stretching uses the analyzed beat window rather than the entire original file
- [ ] Settings -> Audio and Mixer Settings scrolls and exposes beat detection scope, start length, end length, and merge policy
- [ ] `Detect from beginning` only enables the start-length control
- [ ] `Detect from end` only enables the end-length control
- [ ] `Detect from average of beginning and end` enables both length controls and the merge policy
- [ ] `Detect from entire sample` greys out both start-length and end-length controls

## UI / State

- [ ] GUI still fits on screen at the target resolution
- [ ] Help dialog scrolls correctly
- [ ] Save State / Load State restores expected session data
- [ ] README and settings reflect the currently implemented behavior
- [ ] Large sample imports do not hitch the GUI while waveform thumbnails are generated asynchronously
- [ ] Playback, pause/resume, and articulation behavior remain unchanged after timing-cache optimization
- [ ] Attack/release fades still sound correct after the envelope-table optimization pass
