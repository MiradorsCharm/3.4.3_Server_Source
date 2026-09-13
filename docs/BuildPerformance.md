# Build performance

This tree is big: ~1,300 translation units (TUs) and ~650k lines of C++.

| directory             | sources | lines  |
| --------------------- | ------: | -----: |
| `src/server/scripts`  |     628 | 296k   |
| `src/server/game`     |     355 | 288k   |
| `src/plugins` (bots)  |     258 |  27k   |
| rest (`common`, …)    |     143 |  36k   |

Wall-clock time is dominated by **header parsing**, not by code generation:
a 100-line bot source file pulls in the same 189 core headers (~74k lines)
as a 1,400-line one. Everything below attacks that.

## What is already fast (do not undo it)

* **Precompiled headers** – `USE_COREPCH=1` / `USE_SCRIPTPCH=1` (both default).
  The bot plugin has its own PCH (`src/plugins/pchdef.h`) holding the core
  header set; the playerbot headers themselves are deliberately *not* in it so
  editing a bot header does not invalidate the PCH. Turning the PCHs off does
  not only slow the build down, it breaks it: the core headers rely on the
  include set the PCH provides.
* **`/MP`** for the Visual Studio / MSBuild generator – MSBuild only
  parallelises across projects, so `cl.exe` has to fan out itself.
* **`Directory.Build.props`** – disables the VS 2022 build-process manager that
  otherwise serialises projects.

## The fast path: `Build-Fast.ps1`

```bat
Build-Fast.bat                          :: Ninja, all cores but one
Build-Fast.bat -Unity -FastDebugInfo    :: + jumbo TUs + /Z7 debug info
Build-Fast.ps1 -Cores 6 -Clean          :: PowerShell directly, fresh tree
```

What it does differently from a stock MSBuild build:

1. **Ninja instead of MSBuild.** Ninja keeps every core busy across project
   boundaries and has far less per-project overhead. In exchange `/MP` is
   *dropped* for Ninja builds – Ninja already runs one `cl.exe` per core, and
   `/MP` on top of that only oversubscribes the machine.
2. **`sccache` / `ccache`** (`WITH_COMPILER_CACHE`, default `AUTO`) – reuse
   object files from previous builds. Raise the cache size, this tree blows
   through the 5 GB default:
   ```bat
   setx SCCACHE_CACHE_SIZE 20G
   ```
3. **`-Unity`** – see below.
4. **`-FastDebugInfo`** – see below.

Binaries land in `build-fast\bin` (single-config Ninja) instead of
`build\bin\RelWithDebInfo`.

## Individual switches

| option | default | what it does |
| --- | --- | --- |
| `WITH_UNITY_BUILD` | `0` | merges the 258 playerbot sources into batches of 8 TUs |
| `WITH_UNITY_BUILD_BATCH_SIZE` | `8` | sources per merged TU |
| `WITH_FAST_DEBUGINFO` | `0` | MSVC `/Z7` instead of `/Zi` |
| `WITH_FASTLINK` | `0` | link with `/DEBUG:FASTLINK` |
| `WITH_COMPILER_CACHE` | `AUTO` | `sccache` (preferred on MSVC) → `ccache` → none |
| `TOOLS` | `1` | `-DTOOLS=0` skips the map/vmap/mmap extractors you rarely rebuild |

All of them work with both the Ninja and the Visual Studio generator, e.g.

```bat
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
  -DWITH_UNITY_BUILD=1 -DWITH_FAST_DEBUGINFO=1 -DWITH_COMPILER_CACHE=AUTO
cmake --build build --config RelWithDebInfo -- /m
```

### `WITH_UNITY_BUILD` (jumbo build)

The 258 bot sources hold only 27k lines between them, so almost all of their
cost is the shared core-header parse. Merging them 8-at-a-time removes ~87% of
that work and stacks with the PCH.

It is opt-in because merged sources share one translation unit and can clash
(two files defining the same file-local helper, macros leaking into the next
file). A scan of the tree found only two such helpers – `strcmpi` and
`strstri`, both defined in several files – and the five sources involved are
excluded from merging automatically (`SKIP_UNITY_BUILD_INCLUSION`).

The same trick does **not** work for `src/server/scripts`: 626 of its 628
sources define file-scope enums with identical names (`Spells` in 306 files,
`Events` in 229, …), so they would collide immediately. Unity builds are
therefore enabled for the plugin only.

### `WITH_FAST_DEBUGINFO` (`/Z7`)

`/Zi` funnels the debug info of every `cl.exe` through the shared
`mspdbsrv.exe` service, which serialises a `/MP` build and is the usual cause
of hung or `C1902`-failing builds. `/Z7` puts the debug info in the object
files instead: fully parallel. Costs: much larger `.obj` files and a slower
link. The linker still emits the normal PDB, so debugging is unchanged.

### `WITH_FASTLINK` (`/DEBUG:FASTLINK`)

Keeps type information in the objects and only assembles a partial PDB, which
makes linking `worldserver.exe` much faster. Debuggers then resolve types on
demand – slightly slower first lookup, otherwise identical.

## Routine advice

* Build `RelWithDebInfo` (the default); `Debug` is slower to compile and to run.
* Keep the build directory on an SSD, and exclude it from real-time antivirus
  scanning – MSVC writes thousands of files and AV is a measurable tax.
* Use an **out-of-source** build dir and keep it between sessions so that only
  what you touched is rebuilt. With `sccache` even a fresh `-Clean` build is
  mostly cache hits.
* `host=x64` toolset is enforced by `CMakeLists.txt`; a 32-bit hosted `cl.exe`
  cannot build this tree (see the comment at the top of `CMakeLists.txt`).
