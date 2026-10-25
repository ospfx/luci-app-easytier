![luci-app-easytier](https://socialify.git.ci/EasyTier/luci-app-easytier/image?custom_description=EasyTier+Installation+Packages+%28IPK+and+APK%29+for+OpenWrt&description=1&font=JetBrains+Mono&forks=1&issues=1&logo=https%3A%2F%2Favatars.githubusercontent.com%2Fu%2F169161851%3Fs%3D200%26v%3D4&name=1&pulls=1&stargazers=1&theme=Auto)

# Development Guide

English | [简体中文](DEVELOPMENT.md)

A contributor guide. After reading it you should be able to answer three questions: where does the code live,
how many places must change to add an option, and how do I verify **without touching a local compiler**.

Port background and the old/new comparison: [docs/C-PORT.md](docs/C-PORT.md). Verification checklist:
[docs/TESTING.md](docs/TESTING.md).

## Core Principles

**Must support OpenWrt 18.06 ~ 26.x; business logic exists once, in C. LuCI only registers menus and forwards requests.**

- 18.06 has no `menu.d` JSON, so menus and template rendering must use Lua1 `entry()` / `template()`.
  `luasrc/controller/easytier.lua` therefore stays, but it may only do two things: register the menu and forward
  requests to `easytierctl`.
- The backend is no longer Lua. The old "write business logic in Lua" rule is void — **do not move logic back into Lua**.
- The frontend stays ES5 + LuCI Lua1 templates: no ES6+, no LuCI JS framework (`view.js` / `form.js` / `rpc.js`).

## Layout and Module Responsibilities

```
repo/
├── src/                          # C backend (the only place business logic lives)
│   ├── include/                  # contract headers + private headers
│   │   ├── etd.h                 #   shared constants: paths, logs, timeouts, error codes
│   │   ├── etd_api.h             #   API contract: et_req/et_res, 28 handler prototypes, route table
│   │   ├── etd_cfg.h             #   config model + argv assembly + toml
│   │   ├── etd_uci.h             #   libuci wrapper
│   │   ├── etd_util.h            #   buf/list/file/exec/log helpers
│   │   ├── etd_proc.h            #   /proc process discovery, spawn, pidfile
│   │   ├── etd_service.h         #   start/stop/restart/status/enable
│   │   ├── etd_api_priv.h        #   (private) helpers shared by handlers
│   │   ├── etd_net.h             #   (private) firewall and network orchestration
│   │   ├── etd_super.h           #   (private) foreground supervision loop
│   │   ├── etd_watchdog.h        #   (private) checkip watchdog
│   │   └── etd_i18n.h            #   C-side po→JSON dictionary
│   ├── lib/                      # reusable layer: schema, cfg, argv, uci, toml, proc, util, detect, i18n
│   ├── daemon/                   # lifecycle: etd_entry(main), service, super, net, watchdog
│   ├── api/                      # HTTP/CLI endpoints: one common layer + 7 handler files + route table
│   └── ctl/                      # easytierctl (thin CLI)
├── luci-app-easytier/            # frontend bridge (Lua1 + ES5 templates)
│   ├── luasrc/controller/easytier.lua   # 129 lines: menu + forwarding
│   ├── luasrc/view/easytier/*.htm       # 6 page templates (status/core/web/log/upload/console)
│   ├── root/etc/init.d/easytier         # 24 lines: start/stop/reload/status each forward one command
│   ├── root/etc/config/easytier         # default UCI (enabled=0 and 6 more)
│   ├── root/etc/uci-defaults/luci-easytier  # ucitrack, enable service, clear caches
│   ├── root/usr/share/easytier/i18n/*.json  # dictionaries for C rendering (generated)
│   ├── root/usr/share/rpcd/acl.d/*.json     # rpcd ACL (UCI easytier + easytierctl exec)
│   └── po/                              # translation sources (zh_Hans + templates/*.pot)
├── easytierd/Makefile            # OpenWrt package: installs easytierd + easytierctl
├── easytier/  easytier-noweb/    # official binary packaging (independent of this plugin)
├── scripts/cloud/                # cloud build system (see below)
├── tests/                        # golden + frontend bridge tests
├── tools/i18n_build.py           # po → JSON dictionary
└── docs/                         # C-PORT.md, TESTING.md
```

Layer boundaries (decide which layer you are in before editing):

| Directory | Only does | Never does |
| --- | --- | --- |
| `src/lib` | pure data and OS access: schema, UCI, argv, toml, /proc, files, exec | no policy decisions ("when to start", "whether to allow") |
| `src/api` | one handler per endpoint: takes `et_req`, fills `et_res` | no long-lived `fork`, no `init.d` logic |
| `src/daemon` | lifecycle, supervision, firewall, watchdog | no HTTP parameter parsing |
| `src/ctl` | argument parsing + `et_api_dispatch()` | no duplicated business logic |
| `luci-app-easytier` | menus, templates, form/UA → `key=value` | no UCI access, no business files, no command construction |

## The Single Source of Truth: the Schema

`et_schema[]` in `src/lib/etd_schema.c` has **120 entries**; one line carries all metadata for one UCI option:

```c
{ .name = "network_name", .kind = ET_K_PASS, .def = NULL,
  .label = "Network Name", .tab = "general",
  .help  = "The network name used to identify this VPN network (--network-name parameter)",
  .core_flag = "--network-name", .core_emit = ET_E_VAL },
```

It drives four things:

1. **UCI load/save**: `et_cfg_load()` reads every field, filling `.def` when missing. `et_cfg_save()` writes only
   values that differ from UCI    (a value equal to `.def` is not written back, otherwise one save would write all 61 schema defaults
   into `/etc/config/easytier`);
2. **argv assembly**: `et_argv_core()` / `et_argv_web()` walk the table **in row order**, so **row order is
   command-line order**;
3. **form rendering**: `config_render` groups by `.tab`, field order = row order;
4. **docs and debugging**: `easytierctl api schema` emits the schema as JSON.

### Adding an option: the standard recipe

Example: add `--foo-bar <value>`.

1. **Add one row to `et_schema[]`**, positioned where you want it on the command line:

   ```c
   { .name = "foo_bar", .kind = ET_K_STR, .def = NULL,
     .label = "Foo Bar", .tab = "privacy",
     .help  = "What foo bar does (--foo-bar parameter)",
     .core_flag = "--foo-bar", .core_emit = ET_E_VAL },
   ```

   `.kind` selects widget and validation (`STR/INT/BOOL/ENUM/LIST/PASS`); `.core_emit` selects the emission rule
   (`ET_E_VAL` value-only-if-present, `ET_E_YES` bare flag unless "0", `ET_E_REPEAT` one per list item,
   `ET_E_CUSTOM` needs code in `emit_core_custom()` in `etd_argv.c`). **When in doubt use `ET_E_VAL`; do not invent semantics.**
2. **argv takes effect automatically** — no change to `etd_argv.c` (unless `ET_E_CUSTOM`) and none to
   `etd_api_config.c` (the form field appears by itself).
3. **Update po**: `.label` / `.help` are msgids and must exist in `po/templates/easytier.pot` and
   `po/zh_Hans/easytier.po`.
4. **Regenerate dictionaries** (mandatory once po changes; `tests/run_web.sh` runs `--check`):

   ```bash
   python3 tools/i18n_build.py
   ```

5. **Add a golden case** if the option affects argv: extend `tests/golden/<case>.uci` and its `.core.args`.
6. **Verify**: `tests/run_host.sh` (argv) + `tests/run_web.sh` (rendering and i18n).

### Form-only tables

Three kinds of things are **not** in the schema and live in `src/api/etd_api_config.c`; editing them must not touch
`etd_schema.c`:

- `et_extras[]`: `et_config` (config.toml textarea) and `uuid` (machine-id input) — they read/write files, not UCI;
- `et_form_def[]`: fields whose `.def` is NULL but the old CBI had a default (`listenermode=ON`, `tcp_port=11010`,
  `default_protocol=-`, `checktime=2`, …). **Initial form values only; they never affect argv**;
- `et_deps[]` / `et_placeholders[]` / `et_enum_label[]` / `et_forward_opts[]`: show/hide dependencies, placeholders,
  enum labels, multi-select options.

## Coding Rules

- **Language**: C11 + POSIX (`-std=gnu11 -D_GNU_SOURCE`). No C++, and no new dependency beyond `libuci` / `json-c`.
- **Indentation**: **tabs** (width 8), continuation lines aligned with tabs; never mix spaces.
- **Warnings**: `-Wall -Wextra -Werror` (`src/Makefile` adds `-Wno-unused-parameter`). **Zero warnings** is the bar —
  do not silence a warning with `#pragma` to "fix" it.
- **Handle every error path**: check every `malloc` / `open` / `fork` / `exec` / `uci_*` result. On failure, log via
  `et_log_to(ET_LOG_CORE, ...)` or return a specific `ET_E*`. **Never silently `(void)` a fallible syscall.**
- **Resources**: single-exit `goto done` or explicit frees; `et_argv_free()` / `et_list_free()` / `et_buf_free()`
  must be paired. Inside a `fork()`ed child only async-signal-safe calls are allowed, and it must `_exit()`, never `exit()`.
- **Strings and commands**: build commands as argv arrays with `execvp/execve`. **Never use `system()` with
  unescaped paths or user input.** If shell semantics are truly required (pipes), use `et_run_shell()` and comment why.
- **Secrets**: keys and passwords (`network_secret`, `vpn_portal_private_key`, OIDC/Webhook secrets, internal token)
  travel via environment variables or UCI, never in argv. When displaying a command line use `et_argv_visible()`
  (it masks them).
- **File writes**: use `et_write_file()` (temp file + `rename`) for atomicity; use `copy_stream()` for cross-filesystem moves.
- **Log wording**: user-facing log/error strings stay in Chinese exactly as before (so old users can compare);
  do not "improve" them in passing.
- **Comments**: only where the *why* is non-obvious. Whenever you keep an old odd behavior on purpose, cite the source
  (e.g. `init.d:513-525`).

## Cloud Build Discipline (mandatory)

### Rule: never compile or test locally

The Windows worktree has no usable cross toolchain; local builds are both untrustworthy and dirty the tree.
**All compilation, linking, unit tests and ipk packaging happen in the CNB cloud-native development environment.**
Locally you only run `scripts/cloud/*`.

### Three-stage workflow

```bash
# 1) start (or reuse) the cloud environment; writes .cloud/ssh.env (ET_SN / ET_SSH / ET_BRANCH)
scripts/cloud/start.sh

# 2) install the toolchain: base tools + OpenWrt SDK + host libubox/ubus/uci built from source
#    idempotent; cold start ~60s depending on mirror speed
scripts/cloud/setup.sh

# 3) daily compile (host gcc, fastest)
scripts/cloud/cc.sh                 # compile only (objs)
scripts/cloud/cc.sh --link          # compile and link easytierd / easytierctl
scripts/cloud/cc.sh --cross         # cross gcc: headers/syntax validation only
scripts/cloud/cc.sh --cross --link  # cross compile and link (needs deps in staging)

# 4) authoritative build: OpenWrt package system produces the ipk (builds its own deps)
scripts/cloud/build.sh              # easytierd only
scripts/cloud/build.sh --luci       # also luci-app-easytier (needs feeds/luci)
```

Scripts can also run remotely: `scripts/cloud/ssh.sh -s < scripts/cloud/setup.sh`.
For parallel work, give each task its own remote build dir to avoid wiping each other:

```bash
ET_BUILD_ID=feat-argv scripts/cloud/cc.sh --link
```

### The environment gets reclaimed: everything must self-heal

- The platform reclaims the cloud environment after roughly **10 idle minutes**;
- A restarted environment is a **brand-new container**: everything under `/workspace` — the toolchain
  (`/workspace/.toolchain`: SDK plus host libubox/ubus/uci) and build artifacts — is gone;
- Therefore every script must be **idempotent and self-healing** and must not assume "I installed it earlier so it is still there":

| Script | Self-healing behavior |
| --- | --- |
| `start.sh` | calls the CNB API, polls 60×5s for `remoteSsh`, writes `.cloud/ssh.env` |
| `ssh.sh` | on rc=255 (connection layer) restarts the environment and retries; `-s` mode spools stdin to a file so it can replay |
| `cc.sh` | probes `gcc`/`make`, runs `setup.sh` when missing; reports a reclaimed environment on sync failure |
| `build.sh` | probes `sdk/scripts/feeds`, runs `setup.sh` when missing |
| `keepalive.sh` | calls `keepalive_once.sh` every `ET_KEEPALIVE_INTERVAL` (default 120s) |
| `keepalive_once.sh` | keeps a persistent SSH master connection (`ssh -M -N -f` + ControlPath) so the environment stays "in use"; restarts it when the heartbeat fails; then checks `/workspace/.toolchain/host/sbin/ubusd` and `gcc`, rerunning `setup.sh` when the container was reset |

Run keepalive in the background:

```bash
scripts/cloud/keepalive.sh &
# or with a custom interval
ET_KEEPALIVE_INTERVAL=120 scripts/cloud/keepalive.sh &
```

Logs go to `.cloud/keepalive.log`.

### Pitfalls (all hit in practice)

1. **The SDK's `target-*/usr/include` is empty by default.** The OpenWrt SDK does not unpack headers of dependency
   packages, so cross builds often fail with `fatal error: json-c/json.h: No such file or directory`.
   Conclusion: **iterate with host gcc (`cc.sh`), validate authoritatively with the package build (`build.sh`)**.
   `cc.sh --cross` only proves "the code itself compiles", not that it links.
2. **tar sync must pass ownership flags.** The local tree is Windows/NTFS; a naive tarball carries bogus uid/gid and
   permissions into the cloud and breaks `make`. The scripts do it correctly:

   ```bash
   tar czf - -C "$ROOT" --owner=0 --group=0 --numeric-owner \
       --exclude=.git --exclude=.cloud --exclude=build --exclude='*.o' . \
     | ssh ... "tar xzf - --no-same-owner --no-same-permissions -C '$REMOTE_SRC'"
   ```

3. **Sync wipes the remote directory first.** Never keep cloud-only files there; put persistent artifacts in
   `/workspace/.toolchain` or a separate directory.
4. **`make -j8` + `ET_BUILD_ID`**: with several agents in parallel, always use distinct build ids.
5. **SDK download uses mirrors**: `downloads.openwrt.org` often times out from the cloud. `setup.sh` falls back
   NJU → USTC → TUNA → Aliyun → Tencent → upstream, and verifies `sha256sums`.
6. **`tar --zstd` may be unavailable**: the script falls back to `zstd -dc | tar -xf -`.
7. **Do not add `-Wno-error` to `cc.sh`.** Zero warnings is the gate; bypassing it just moves the failure to the
   package build.

## Tests

Two entry points, both executed in the cloud (locally you only trigger them):

```bash
bash tests/run_host.sh    # host build + unit/UCI/save-roundtrip + golden argv comparison
bash tests/run_web.sh     # controller syntax+forwarding, template Lua/JS syntax, i18n check, C rendering tests
```

- `tests/run_host.sh`: sync → `cc.sh` (a failure is non-fatal, it continues with a module-only build) → compile
  `tests/argv_test` (covers `etd_util/uci/schema/cfg/argv/toml/detect`) → run `tests/golden`.
  11 sample cases plus unit/UCI/round-trip assertions total **54 items** (the authoritative number is the
  `== 结果: N 项通过, M 项失败 ==` line it prints). `*.notes` files assert the explanatory text for intentional
  differences (e.g. an old-style vpn_portal being skipped).
- `tests/run_web.sh`: six steps — `luac -p` on the controller, `bridge_lua_test.lua` (fake `luci.*`, all 28 endpoints:
  command line, MIME, JSON body, upload spooling, menu structure), template `<% %>` Lua blocks and `<script>` ES5 syntax,
  `tools/i18n_build.py --check`, then compile/run `tests/web_bridge_test`
  (config_get/save/render/schema + i18n).

Detailed commands, single-step reproduction and the pre-device checklist: [docs/TESTING.md](docs/TESTING.md).

## Submitting Changes

1. **Pass the three gates in the cloud first**: `scripts/cloud/cc.sh --link` (zero warnings), `tests/run_host.sh`,
   `tests/run_web.sh`. If you touched packaging, also run `scripts/cloud/build.sh --luci`.
2. **Keep route names identical to the old endpoints.** Names in `src/api/etd_routes.c` (`api_status`, `get_log`,
   `upload_binary`, …) *are* the LuCI URLs used by `url("admin","vpn","easytier","<name>")` in the templates.
   Renaming means changing the frontend and breaking 18.06 compatibility — **unless you update the templates in the
   same change and justify it**. New routes must also be added to the `EP` list in `luasrc/controller/easytier.lua`
   and, if they need new privileges, to `root/usr/share/rpcd/acl.d/luci-app-easytier.json`.
3. **Contract headers are append-only.** `src/include/etd*.h` are the module boundaries (7 contract headers + 5 private):
   - you may add functions, enum values (at the end) and struct fields (at the end);
   - **do not remove, rename or reorder** existing functions, fields or enum values — `src/api`, `src/daemon`,
     `src/ctl` and `tests/` (including stubs such as the empty route table in `web_bridge_test.c`) all depend on them;
   - when adding an enum value, check every `switch` for a `default`, otherwise `-Wswitch` becomes an error.
4. **Record behavior differences.** Anything that differs from the old implementation needs a "why" comment in code
   or an entry in the difference list of [docs/C-PORT.md](docs/C-PORT.md). Code and docs must not disagree.
5. **Do not refactor on the side.** A bug fix touches only the lines it needs; formatting, renames and file splits go
   in separate PRs.
6. **Do not commit build output**: `build/`, `*.o` and `.cloud/` are ignored.

## FAQ

**Why keep any Lua at all instead of going full LuCI2/JS?**
18.06 has no `menu.d` JSON: menus and templates can only be rendered by Lua1, and a JS framework is a poor fit for
low-end routers. Keeping 129 lines of Lua is the cheapest compatibility option; see
[docs/C-PORT.md](docs/C-PORT.md).

**Why does the status page still spawn processes?**
Only three necessary cases remain: `easytier-core -V` / `easytier-cli <part>` (version and connection info), the
GitHub latest-tag query, and an `ip addr` fallback when `ifconfig` is missing. CPU, memory, traffic and disk all come
from `/proc`, `sysfs` and `statvfs`.

**Why are unknown fields silently ignored in `config_save`?**
So a frontend/backend version mismatch degrades gracefully (an old template submitting an extra field must not fail the
whole save). Only validation failures (bad type/enum) return an error.

**Can I at least run `gcc -fsyntax-only` locally?**
No. Without libuci/json-c headers and the correct include paths the result is meaningless — use `scripts/cloud/cc.sh`.

## References

- [LuCI Lua1 API](https://github.com/openwrt/luci/wiki)
- [OpenWrt UCI](https://openwrt.org/docs/guide-user/base-system/uci)
- [EasyTier CLI flags](https://easytier.cn)
- [ES5 reference](https://www.ecma-international.org/ecma-262/5.1/)
