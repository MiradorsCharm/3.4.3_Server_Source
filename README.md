# World of Warcraft 3.4.3 Source Source code.

## Prerequisites

OpenSSL 3.5.7 - [Download](https://slproweb.com/download/Win64OpenSSL-3_4_6.exe)

Visual Studio 2022 Community - [Download](https://aka.ms/vs/17/release/vs_community.exe)

Boost 1.83 - [Download](https://archives.boost.io/release/1.83.0/binaries/boost_1_83_0-msvc-14.3-64.exe)

> **Any Boost from 1.78 upwards works**, including current releases. Boost 1.86
> moved the Boost.Process v1 headers under `boost/process/v1/` and Boost 1.88
> removed the old top-level paths; `src/common/Utilities/StartProcess.cpp`
> selects the right set via `BOOST_VERSION`, and `dep/boost/CMakeLists.txt`
> defines `BOOST_PROCESS_VERSION=1` so the v1 API stays visible. Boost 1.87 also
> stopped pulling `<boost/filesystem/directory.hpp>` in from
> `<boost/filesystem/operations.hpp>` and dropped `<boost/asio/io_service.hpp>`;
> both are handled in the source as well.


Latest version of CMake - [Download](https://cmake.org/download/)

MySQL 8.0 - [Download](https://dev.mysql.com/downloads/windows/installer/8.0.html)

3.4.3 Client - [Download](https://drive.google.com/file/d/1rbu3qp2AIk6j2VFmx2yAb8nTIANRZLqp/view)

## AI Playerbots

This source tree includes a port of [ike3/mangosbot](https://github.com/ike3/mangosbot)
AI Playerbots, living in `src/plugins/playerbot`.

Quick start:

1. Build normally — the `plugins` library is part of the CMake build.
2. Import `sql/custom/playerbot/characters_playerbot.sql` into your **characters** database.
3. Configure bots in the **AI PLAYERBOT SETTINGS** section of `worldserver.conf`.
   All defaults are included in the normal `worldserver.conf.dist` copied by the build
   (`COPY_CONF=1`, the default). For an existing server, merge that section into your
   live `worldserver.conf`; rebuilding never overwrites your live configuration.
   Keep `AiPlayerbot.Enabled = 1` to enable bots and restart worldserver.
   No separate `aiplayerbot.conf` is needed. Migrate any old overrides from
   `worldserver.conf.d/aiplayerbot.conf`, then remove that old override file.
4. In game: `.bot add <charactername>`, then whisper the bot `follow`, `attack my target`, `stay`, …

See [docs/Playerbots.md](docs/Playerbots.md) for the full documentation:
architecture, hook points, configuration reference, chat commands, random bot
population management and the 3.3.5 → 3.4.3 porting notes.

## Continuous Integration & Releases

This repository uses GitHub Actions:

* **Build workflow** (`.github/workflows/build.yml`) — automatically builds the
  project on every push to `main` and on every pull request, verifying that the
  server and the playerbot plugin compile and link correctly with MSVC on
  Windows Server 2022.  Windows is the only CI target and the job is blocking;
  a failure posts the extracted compiler errors back to the pull request.
* **Release workflow** (`.github/workflows/release.yml`) — triggered manually
  from the **Actions** tab (`Run workflow`).  Builds the full server with
  playerbots for Windows x64, packages the binaries together with the SQL
  schemas, configuration files, documentation and every required DLL, and
  creates a GitHub Release with a downloadable `.zip`.
* **Dependencies** are installed with the vcpkg copy that ships with the GitHub
  runner image. It is *deliberately not pinned* to an older release: an old
  vcpkg asks the MSYS2 mirrors for package revisions that were deleted upstream
  (`msys2-runtime-3.5.4-2`), so every port running a pkgconfig fixup — bzip2,
  and openssl — fails with HTTP 404 and the dependency step dies. Keeping vcpkg
  current and supporting modern Boost in the source is the only combination that
  stays green over time.

## Building (Windows only)

**This tree builds on Windows with MSVC only.** Linux / macOS support was
removed on purpose — the Unix build files, compiler settings and the Linux CI
job are gone, and `cmake` now stops with a clear error if you configure it on
anything but Windows. If you need a Unix build, use upstream TrinityCore.

Install the prerequisites listed at the top of this file, then from a
*Developer Command Prompt for VS 2022* (or with CMake GUI):

```bat
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DSERVERS=1 -DTOOLS=1 -DSCRIPTS=static ^
  -DWITH_DYNAMIC_LINKING=0 ^
  -DUSE_COREPCH=1 -DUSE_SCRIPTPCH=1

cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo --prefix C:\TrinityCore
```

> **Keep the precompiled headers enabled** (`USE_COREPCH=1`, `USE_SCRIPTPCH=1`,
> which are the defaults). The core headers rely on the include set the PCHs
> provide; with `-DUSE_COREPCH=0` MSVC 14.4x no longer pulls most of the
> standard library in transitively and the build dies in headers that are
> otherwise fine, e.g. `SharedDefines.h: error C2039: 'unordered_map': is not a
> member of 'std'` followed by thousands of cascading errors.

### Faster builds

The full build is ~1,300 translation units and is dominated by header parsing.
Two things help most: a real precompiled header for the bot plugin (already on
by default) and a faster generator + compiler cache.

```bat
Build-Fast.bat                        :: Ninja + sccache + PCH, all cores but one
Build-Fast.bat -Unity -FastDebugInfo  :: additionally merge bot sources and use /Z7
```

See [docs/BuildPerformance.md](docs/BuildPerformance.md) for the individual
CMake switches (`WITH_UNITY_BUILD`, `WITH_FAST_DEBUGINFO`, `WITH_FASTLINK`,
`WITH_COMPILER_CACHE`) and what each one trades away.

### Troubleshooting

**"Error: generator toolset: Does not match the toolset used previously: host=x64"**

This means CMake is finding a stale `CMakeCache.txt` in your **build directory**
that was created by an earlier configure with different settings.  Removing and
re-cloning the *source* directory does not fix it — the cache lives in the
build directory (the folder you passed to `-B`), not in the source tree.

Fix: delete the build directory and start over:

```bat
rmdir /s /q build
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ...
```

Or, if you are using the CMake GUI, change the **build directory** path to a
fresh, empty folder.  The source directory should point to the cloned source
tree; the build directory must be a separate, empty folder.

**bnetserver: `OSSL_STORE_open failed ... / Failed to initialize SSL context`**

```
Using configuration file E:/Wotlk/Bots/bin/bnetserver.conf.
OSSL_STORE_open failed: The system cannot find the file specified
Failed to initialize SSL context
```

bnetserver serves the login and Battle.net REST endpoints over TLS, so it needs a
certificate before it opens a socket and exits if it cannot read one.  This is not
a broken certificate - the ones in this tree (`CN = *.*`, TrinityCore CA) are valid
until 2036 - it is `CertificatesFile = "./bnetserver.cert.pem"` not resolving to a
file.  The default is relative to the *working directory*, and the file lives in
`src\server\bnetserver\` in the source tree (the build copies it next to the
executables in `build\bin\<Config>\`).

Fix, any of:

```bat
:: 1. put the certificate and key next to bnetserver.exe
copy src\server\bnetserver\bnetserver.cert.pem E:\Wotlk\Bots\bin\
copy src\server\bnetserver\bnetserver.key.pem  E:\Wotlk\Bots\bin\

:: 2. or point the config at them (absolute path, either separator)
::    bnetserver.conf:  CertificatesFile = "E:/Wotlk/Bots/bin/bnetserver.cert.pem"

:: 3. or have bnetserver create its own self-signed key pair on the next start
::    bnetserver.conf:  GenerateSelfSignedCertificate = 1
```

Option 3 writes `bnetserver.cert.pem` and `bnetserver.key.pem` (2048 bit RSA,
SHA-256, ten years) to the configured paths and reuses them afterwards; it is meant
for a private server and never overwrites an existing key.

If `bnetserver.conf` sits in an `etc\` directory next to `bin\`, start the server
from `bin\` (the `start-bnetserver.bat` launcher in the release zip does that) so
the certificate, the config and the `data\` files all resolve.  worldserver itself
does not use TLS; the login failures you see there are just the fallout of
bnetserver never having started.

A rebuilt bnetserver from this source tree also prints every directory it searched
and no longer requires the working directory to be the one containing the `.pem`
files, so this mistake is both harder to make and obvious when it happens.

**`The code execution cannot proceed because openssl_ed25519.dll was not found`**

`dep/openssl_ed25519` (hotfix signature verification) is the one vendored
dependency that is built as a **shared** library no matter what
`WITH_DYNAMIC_LINKING` says, so `worldserver.exe` and `bnetserver.exe` both import
`openssl_ed25519.dll` and load it from their own directory.  The loader fails
before `main()` runs, which is why the console window just flashes and nothing
reaches the log.  Copy the DLL - and in general every `.dll` the build put next to
the executables - into the directory holding the `.exe` files:

```bat
copy build\bin\RelWithDebInfo\*.dll  E:\Wotlk\Bots\bin\
```

The release zip ships them in `bin\`; only files that were moved out of `bin\` by
hand can produce this error.

**Client data: `dbc`, `maps`, `vmaps`, `mmaps`**

The extraction tools in `bin\` all write to the **current working directory** and
each takes the client directory under a different flag:

```bat
cd /d E:\Wotlk\Bots\bin
mapextractor.exe    -i "C:\World of Warcraft"     :: dbc, maps, Cameras, gt
vmap4extractor.exe  -d "C:\World of Warcraft"     :: writes .\Buildings
vmap4assembler.exe  Buildings vmaps                :: two plain arguments
mmaps_generator.exe                                :: reads .\vmaps, writes .\mmaps
```

`mapextractor` has no `-d` and `mmaps_generator` takes no client directory at all,
so running them from anywhere else either fails on the command line or leaves the
data in the wrong place.  `Extract-ClientData.bat` in the release zip does the
`cd` and the four steps for you.  `DataDir = "."` then finds everything, from
whichever directory worldserver is started: a relative `DataDir`, `LogsDir`,
`IPLocationFile` and the TLS certificate files are resolved against the working
directory, the directory of the executable and the directory of the config file,
and a missing log directory is created.  When nothing matches, the server names the
paths it searched instead of reporting a few thousand unreadable files.

## The release-zip workflow, end to end

The Windows release artifact is meant to be usable without building anything:

1. unzip, double-click `Setup-Database.bat`.  It downloads a portable MariaDB into
   `database\`, creates `auth`, `characters`, `world` and `hotfixes`, downloads the
   world and hotfixes dumps (SHA-256 verified), applies `sql\updates`, writes
   `etc\worldserver.conf` and `etc\bnetserver.conf` with the database credentials
   filled in, and seeds the realm row in `auth.realmlist` that `sql\base` does not
   ship (without it bnetserver hands out an empty realm list and the login stops
   after the password).  The database stays running.
2. extract the client data once: `Extract-ClientData.bat "C:\World of Warcraft"`,
   or copy an existing `dbc`, `maps`, `vmaps`, `mmaps` set into `bin\`.  This step is
   required, not optional: `worldserver.exe` needs `dbc` (the DB2 stores) and
   `vmaps\gameobjects.raw` and exits when they are missing - it now says which
   directory it resolved them from instead of failing on a bare file name.
3. `start-bnetserver.bat`, then `start-worldserver.bat`.  Both `cd` into `bin\` and
   start the portable MariaDB first if nothing is listening (a reboot stops it),
   so the only prerequisite is step 1 and 2.
4. in the worldserver window: `bnetaccount create you@example.com yourpassword`,
   then `account set seclevel <the game account name it prints> 2`.  Battle.net
   accounts are named by e-mail address, hence the `@`.

Where things have to be, and what checks them:

* `etc\*.conf` is the only configuration the servers read; `ConfigMgr` searches the
  working directory, the executable directory and `..\etc`, and `Trinity::DataPaths`
  (`src/common/Utilities/DataPaths.h`) resolves every other relative path
  (`DataDir`, `LogsDir`, `IPLocationFile`, `CertificatesFile`, `PrivateKeyFile`)
  through the same list, creates a missing `LogsDir`, and names the paths it tried
  when nothing matches.  A missing log file or packet log is reported once, on
  stderr, because the appenders exist before any logger does.
* `bin\` must keep `worldserver.exe`, `bnetserver.exe`, all `.dll` files (including
  `openssl_ed25519.dll` and `libmysql.dll`, both loaded from the executable
  directory) and `bnetserver.cert.pem`/`bnetserver.key.pem`.  The release job fails
  if any of those is not in the package.
* the client build is 3.4.3.54261; `auth.build_info` has to carry a row for it
  (`sql\updates/auth/3.4.3/2026_08_10_00_auth.sql`, re-seeded by the setup script if
  absent), otherwise every login is refused with `ERROR_BAD_VERSION`.
* if the client logs in but the realm never connects, the realm name it was handed
  is `Region-Battlegroup-RealmID` (`1-1-1` for the seeded row) and it resolves that
  itself: add `127.0.0.1  1-1-1` to `C:\Windows\System32\drivers\etc\hosts`, or put
  the real address of the machine in `auth.realmlist.address` for LAN play.
