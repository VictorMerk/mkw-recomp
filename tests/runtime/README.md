# tests/runtime

Pieces of `runtime/` built alone: audio, controllers, memory caps, settings and the menu's texts.

| File | What it checks |
|---|---|
| `test_memwatch.py` | Native Windows process-memory collection: observes an allocated 64 MiB increase, preserves logs and exits with its owned target; no game or GPU |
| `test_gamelib_id.py` | Canonical and legacy CRLF header hashes, invalidation after header/flag changes, and production library-guard rejection with synthetic data |
| `test_lan_peers.py` | Production socket overrides with owned UDP endpoints: opt-in, peer filtering and refresh, loopback binding, broadcast fanout, direct-send isolation, public-address rejection and timestamped diagnostics; Linux and Windows |
| `test_ui_text.py` | The menu's text catalogs (`i18n/`): the English template has every id of `ui_text.h`, each `.po` is up to date with it, and each translation keeps the English's printf conversions in order (a swapped `%s`/`%d` would make printf read the wrong argument) |
| `test_switch2.py` | The Switch 2 controllers' Bluetooth LE protocol (`runtime/input/switch2_proto.h`): mkw-neo's test (`t_switch2.c`, reports recorded from real Joy-Con 2s) built against our copy |
| `test_cfg.py` | Every setting the in-game menu writes to `mkw.ini` is a key `runtime/core/base/cfg.c` reads back (#168: `HDR` was dropped as unknown) |
| `test_defaults.py` | The memory caps a player gets with nothing set (`runtime/gfx/vk/device/vk_defaults.c`), built alone and checked on the GPUs players reported: the Radeon 660M keeps its measured caps, a large discrete GPU is not held to them (issue #31) |
| `test_audio_update.py` | The actual audren RequestUpdate parser (`runtime/audio/audren.c`, its section cut out and built alone) with synthetic REV11/REV15 records: zeroed unused slots cannot reset playing voice 0, freed slots stop, reused slots reset and output status stays with its slot |
| `test_adpcm.py` | DSP-ADPCM voices (`runtime/audio/audio_mixer.c`, its voice decoding cut out and built alone) against an exact decode of random buffers (`t_adpcm.c`): with a context on every buffer, loops included, the output is the exact decode; without contexts at most a few samples differ (a slot refilled before the walk decoded its last samples); high pitch and sample rate cross more than 128 input samples at once; each sample is decoded about once (#331) |

C sources the tests here build with the compiler at hand and run:

| File | Built by | What it checks |
|---|---|---|
| `t_gamelib_id.c` | `test_gamelib_id.py` | Actual library guard with synthetic tables and sidecars: canonical/CRLF ids accepted; incompatible ids, ABI and table bounds rejected |
| `t_lan_peers.c` | `test_lan_peers.py` | Single-thread production socket overrides with synthetic guest registers and owned loopback UDP endpoints; no external traffic |
| `t_switch2.c` | `test_switch2.py` | The Switch 2 controllers' protocol (`runtime/input/switch2_proto.h`): from seedds/mkw-neo (MIT), with reports and replies recorded from a left and a right Joy-Con 2 |
| `t_audio_update.c` | `test_audio_update.py` | Voice slots in RequestUpdate: `audren.c`'s update section (`request.inc`, cut out by the test) with the mixer and platform stubbed |
| `t_adpcm.c` | `test_adpcm.py` | DSP-ADPCM voices: `audio_mixer.c`'s voice decoding (`voice.inc`, cut out by the test) against an exact decode of the same random buffers, five sample rates, seven pitches up to 3.9 (plus a high-rate case at 192 kHz and pitch 64), with and without contexts; prints output samples, wrong samples and decodes |
