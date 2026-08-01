## Enabling

Configure with `-DAUTOMATION=ON`. The option is off by default and must stay off in packaged builds; it compiles a loopback-only TCP control API into the client that can execute arbitrary console commands. Combine with `-DHEADLESS_CLIENT=ON` to drive the client without a display or GPU, which is how `scripts/integration_test.py --test-automation` runs it.

The client opens the API when `cl_automation_port` is non-zero (`0` = disabled, the default). `cl_automation_fixed_step` switches `time_get()`/`time_get_nanoseconds()` to a virtual clock that only advances by that many microseconds per frame, removing OS scheduling jitter from timing; frame pacing (`beNice`, `cl_refresh_rate`) still uses the real clock, so this does not decouple the client from a real-time server. `cl_refresh_rate_inactive` defaults to `120` and overrides `cl_refresh_rate` whenever the window is inactive, which is always true under `-DHEADLESS_CLIENT=ON`; leaving it unset makes virtual time run at roughly 2.4x real time and breaks the fixed step's determinism. `cl_automation_fixed_step` is applied only when `cl_automation_port` is non-zero. Set these together:

```
cl_automation_port 7801
cl_automation_fixed_step 20000
cl_refresh_rate 50
cl_refresh_rate_inactive 50
```

On success the client logs `automation: listening on 127.0.0.1:<port>`; a harness should wait for that line rather than retry-connecting.

Neither variable is `CFGFLAG_SAVE`, so the port is never persisted into `settings_ddnet.cfg`. The API accepts exactly one connection; a second connection attempt is refused and closed.

## Protocol

Line-delimited JSON over TCP, UTF-8, one object per line, `\n`-terminated. All numeric values in the protocol are integers: the quantities that matter (position, velocity in 1/256 units, hook direction in 1/256 units) are already integers in the domain the client and server are required to agree on, so there is no float writer.

Request:

```json
{"id": 17, "cmd": "console", "args": {"line": "cl_predict 1"}}
```

`id` is a required integer, echoed back. `args` is optional; omitting it is equivalent to `{}`.

Success reply:

```json
{"id":17,"ok":true,"frame":1234,"game_tick":8123,"predicted_tick":8129,"result":{}}
```

Error reply:

```json
{"id":17,"ok":false,"frame":1234,"game_tick":8123,"predicted_tick":8129,
 "error":{"code":"unknown_command","message":"unknown command 'foo'"}}
```

- `frame` increments once per client frame, starting at 0.
- `game_tick` / `predicted_tick` are `-1` when the client is not `STATE_ONLINE`.
- Issue one request at a time. `wait_ticks`/`wait_for` defer their reply while every other command replies within the frame, so a pipelined batch completes out of order.
- Connection-level rejections (a second client connecting, an over-long line, invalid UTF-8) carry `"id": -1` and no `frame`/`game_tick`/`predicted_tick` envelope.
- `error.code` is one of: `parse_error`, `bad_request`, `unknown_command`, `bad_args`, `not_online`, `no_character`, `timeout`. A `parse_error` reply carries `"id": -1` when the id itself could not be parsed.
- A request is applied at the top of the frame it arrives in, and its reply is emitted after that same frame's network pump, prediction and render have completed, so `wait_ticks` and `get_state` need no polling or sleeping on the driver side.
- `ping` returns `{"protocol": 1, "version": "<GAME_RELEASE_VERSION>"}`. A driver should check `protocol == 1` on connect and refuse to proceed otherwise.

## Commands

| Command | Args | Result |
| --- | --- | --- |
| `ping` | — | `{"protocol": 1, "version": string}` |
| `console` | `{"line": string}` | `{}`. Dispatches the line to the console; the console has no return value, so this reports dispatch, not success. Read the client's stdout for console output. |
| `get_config` | `{"name": string}` | `{"name": string, "type": "int"\|"color"\|"string", "value": int\|string}` |
| `get_status` | — | client/game state, see below |
| `wait_ticks` | `{"ticks": int, "timeout_frames": int (default 3000)}` | `{}`. Blocks the reply until `predicted_tick` has advanced by at least `ticks`. |
| `wait_for` | `{"predicate": string, "timeout_frames": int (default 3000), ...}` | `{}`. Blocks the reply until the named predicate is satisfied; see below. |
| `key_down` / `key_up` | `{"key": string}` or `{"code": int}` | `{}`. Injects a synthetic key event. |
| `key_press` | same as `key_down` | `{}`. A `key_down` immediately followed by `key_up` in the same frame. |
| `text` | `{"text": string}` | `{}`. Injects a synthetic text-input event; `text` must be valid UTF-8. |
| `mouse_move` | `{"dx": int, "dy": int}` | `{}`. Injects a synthetic relative mouse delta. |
| `set_input` | `{"dummy": 0\|1, "dir": -1\|0\|1, "jump": 0\|1, "hook": 0\|1, "fire": 0\|1, "target_x": int, "target_y": int, "weapon": 0..5, "next_weapon": int, "prev_weapon": int}`, all optional | `{}`. Overrides scripted gameplay input for one dummy slot; unset fields keep their previous scripted value. |
| `clear_input` | `{"dummy": 0\|1 (default: current)}` | `{}`. Stops overriding input for that slot; real input resumes on the next tick. |
| `get_state` | — | current predicted/snapshot character core, plus last scripted input, see below |
| `get_parity_history` | `{"max": int (default 400)}` | drains and returns up to `max` buffered `{tick, predicted, snapshot}` entries |

`wait_for` predicates:

| predicate | extra args | satisfied when |
| --- | --- | --- |
| `state` | `{"value": "<state name>"}` | client state matches (`offline`, `connecting`, `loading`, `online`, `demoplayback`, `quitting`, `restarting`) |
| `game_tick_at_least` | `{"value": int}` | authoritative game tick `>= value` |
| `predicted_tick_at_least` | `{"value": int}` | predicted tick `>= value` |
| `has_local_character` | — | the last pushed snapshot has an active local character |
| `map_loaded` | `{"name": string}` | the loaded map's base name equals `name` |

`get_status` result:

```json
{"state": "online", "prev_game_tick": int, "tick_speed": 50, "local_time_ms": int,
 "frame_time_us": int, "server_address": string, "map_name": string,
 "local_client_id": int, "dummy": int, "dummy_connected": bool,
 "menus_active": bool, "editor_active": bool, "connection_problems": bool,
 "fixed_step_us": int}
```

`menus_active` reports `PLAYERFLAG_IN_MENU` only; chat sets `PLAYERFLAG_CHATTING` instead and is not covered by this field. It also reads `false` until the first `SnapInput` of an online session. It matters for scripted input because while the menu is open, `SnapInput` strips `PLAYERFLAG_PLAYING` and resets input, so `set_input` has no observable effect. Check `menus_active` before relying on scripted movement.

`get_state` result:

```json
{"local_client_id": int,
 "predicted": {"tick": int, <14 core fields>, "active_weapon": int, "jumps": int,
               "jumped_total": int, "freeze_start": int, "freeze_end": int,
               "solo": bool, "jetpack": bool, "collision_disabled": bool, "endless_hook": bool,
               "endless_jump": bool, "hammer_hit_disabled": bool, "grenade_hit_disabled": bool,
               "laser_hit_disabled": bool, "shotgun_hit_disabled": bool, "hook_hit_disabled": bool,
               "super": bool, "invincible": bool, "deep_frozen": bool, "live_frozen": bool} | null,
 "snapshot": {"tick": int, <14 core fields>, "player_flags": int, "health": int, "armor": int,
              "ammo_count": int, "weapon": int, "emote": int, "attack_tick": int},
 "prediction_for_snapshot_tick": {"tick": int, <14 core fields>} | null,
 "input": {"direction": int, "target_x": int, "target_y": int, "jump": int, "fire": int,
           "hook": int, "player_flags": int, "wanted_weapon": int, "next_weapon": int,
           "prev_weapon": int}}
```

`predicted` is `null` when prediction is disabled or not yet valid for the current tick. `get_state` fails with `not_online` / `no_character` when there is no active local character.

The 14 core fields, shared by `predicted`/`snapshot`/`prediction_for_snapshot_tick` and by `get_parity_history`'s entries, are the `CNetObj_CharacterCore` fields the client and server are required to agree on exactly: `x, y, vel_x, vel_y, angle, direction, jumped, hooked_player, hook_state, hook_tick, hook_x, hook_y, hook_dx, hook_dy`. `m_Tick` is excluded because the server writes `0` into the snapshot character's `m_Tick` field (it is a dead-reckoning tick, not the game tick).

## Python driver

`scripts/automation.py` is a stdlib-only client for the protocol above (`AutomationClient`), used by `scripts/integration_test.py`. `Client(..., automation_port=<port>)` launches a client with every config value listed under "Enabling" already set, plus `cl_antiping 1` and `cl_antiping_weapons 1`, and `Client.automation(port)` waits for the listening log line and returns a connected `AutomationClient`.

`scripts/integration_test.py automation_prediction_parity` (gated behind `--test-automation`) connects a headless client to a server running `data/maps/coverage.map`, scripts forward+jump movement, and asserts the predicted and authoritative character cores are exactly equal, tick by tick, over a 60-tick window. It is exact integer equality with no tolerance: any mismatch is a real divergence between `src/game/server/entities/` and `src/game/client/prediction/entities/`.

Note: the test pins `cl_antiping 1` and `cl_antiping_weapons 1`. With antiping off (the client default), the client does not predict `CDoor` collision at all (`m_WorldConfig.m_PredictWeapons` gates it in `CGameWorld::NetObjAdd`), so it walks straight through doors the server treats as solid. That is a genuine client bug, not a test artifact; it is tracked separately and deliberately not fixed here. Pinning antiping is what makes this test measure physics divergence instead of that one known gating defect.

## Writing tests

### Assert on `snapshot`, not `predicted`

Gameplay assertions belong on `snapshot`, the server's authoritative state. On a client that mispredicts, asserting on `predicted` passes while the real game did something else, which is the failure this API exists to catch. Use `predicted` only for parity comparisons.

### Do not sleep

`wait_ticks` and `wait_for` block in-engine and reply once the condition holds, so tests read as a straight sequence and never poll. A test that sleeps is both slower and flakier.

### Convenience helpers

`AutomationClient` provides these on top of the raw commands:

| Helper | Replaces |
| --- | --- |
| `connect(port)` | console connect, wait online, wait for local character, settle, drain parity history |
| `snapshot()` / `predicted()` | `get_state()["snapshot"]` / `["predicted"]` |
| `hold(ticks, **inputs)` | `set_input(...)`, `wait_ticks(...)`, read the result |
| `sample(ticks, every=N)` | a loop collecting snapshots across a window |
| `parity_mismatches()` | drain the history, keep only disagreeing ticks |

### Choosing a map

Movement room varies enormously and the wrong map yields a test that cannot fail. Measured over 30-tick windows from spawn:

| Map | Walk right | Jump rise | Hook |
| --- | --- | --- | --- |
| `coverage` | 65 units | 3 units | never latches |
| `Tutorial` | 240 units | 173 units | latches downward |
| `LearnToPlay` | 240 units | 115 units | latches downward |

`coverage.map` is a synthetic tile-coverage course whose spawn is boxed in; it is the right map for exercising DDRace entity classes and the wrong one for anything about movement.

### Coordinates and hooking

`y` grows downward, so a jump lowers it and the apex is `min(y)` over the airborne window. Positions and velocities are quantized `CNetObj_CharacterCore` values, with velocity in 1/256 units per tick. `m_Jumped` stays set for the whole airborne window.

Aiming the hook at open air only reaches `HOOK_FLYING`; hooking straight down into the floor the tee stands on latches reliably. The hook also latches within a few ticks, so coarse sampling can miss `HOOK_FLYING` entirely — assert on the terminal `HOOK_GRABBED`. Hook states are defined in `src/game/gamecore.h`.

### Thresholds

Derive them from a measurement and record it in a comment. A threshold tuned until the test passes is worse than no test.

### Verify the test can fail

Confirm the failure path deliberately. `automation_prediction_parity` was checked by dropping `cl_antiping`, which makes it report mismatching ticks with the door-prediction signature described above.

### Gotchas

- `wait_for_startup([client, server])` races the handshake: the client logs `automation: listening` *before* `client: version`, and the log queue has a single consumer, so waiting for both consumes the automation line and then blocks. Wait on the server only; `client.automation(port)` has its own later readiness signal.
- Use a distinct port per run, ideally `free_tcp_port()`.
- Only one automation client may be connected at a time; a second receives `bad_request`.
- `console screenshot` returns `ok` and writes nothing in a `-DHEADLESS_CLIENT=ON` build, because the null backend has no framebuffer. There is also no command to retrieve an image over the socket.
- Kill every process a test starts. Strays hold ports and silently corrupt later runs.

## Security

The listen socket is hard-coded to `127.0.0.1` and the feature is compiled out unless `-DAUTOMATION=ON` is passed, with the port defaulting to `0` (disabled) even then. There is no authentication: any local process that can reach the port gets full console access, the same trust model as `cl_input_fifo`. Do not enable this option in a build that is exposed to untrusted local users.
