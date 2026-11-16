# tests/c

C sources the Python tests next door build with the compiler at hand and run.

| File | Built by | What it checks |
|---|---|---|
| `t_gamelib_id.c` | `test_gamelib_id.py` | Actual game-library guard with synthetic tables and sidecars; canonical/CRLF ids accepted, incompatible ids/ABI/table bounds rejected; no game or GPU |
| `t_lan_peers.c` | `test_lan_peers.py` | Single-thread production socket override harness with synthetic guest registers and owned loopback UDP endpoints; no game, GPU or external traffic |
| `t_spirv_images.c` | `test_spirv_images.py` | Runs the actual integer-image shader patcher on synthetic modules, verifies input preservation and the failure-result contract |
| `t_switch2.c` | `test_switch2.py` | The Switch 2 controllers' protocol (`runtime/input/switch2_proto.h`): from seedds/mkw-neo (MIT), with reports and replies recorded from a left and a right Joy-Con 2 |
| `t_game_dir.c` | `test_game_dir.py`, `test_game_dir_windows.py` | Settings, installation discovery and log-folder discovery with real files and single-thread platform stubs; Windows uses UTF-16 file APIs and UTF-8 conversion through `wmain`; no game, SDL or GPU |
| `t_adpcm.c` | `test_adpcm.py` | DSP-ADPCM voices: `audio_mixer.c`'s voice decoding (`voice.inc`, cut out by the test) against an exact decode of the same random buffers, five sample rates, seven pitches up to 3.9 (plus a high-rate case at 192 kHz and pitch 64), with and without contexts; prints output samples, wrong samples and decodes |
