# Playerbots (ported from ike3/mangosbot trinity-wotlk-ai) plus the minimal
# auction-bot pricing support used by the bots.
#
# IMPORTANT: The playerbot plugin calls many core functions that are not
# marked TC_GAME_API.  When WITH_DYNAMIC_LINKING is ON the core libraries
# use -fvisibility=hidden and those symbols become unavailable.  The
# plugin therefore requires static linking (WITH_DYNAMIC_LINKING=OFF).

if(BUILD_SHARED_LIBS)
  message(WARNING
    "  Playerbot plugin: BUILD_SHARED_LIBS is ON (WITH_DYNAMIC_LINKING=1).\n"
    "  The bot code calls core functions that lack TC_GAME_API exports,\n"
    "  so the build will almost certainly fail to link.  Reconfigure with\n"
    "    -DWITH_DYNAMIC_LINKING=0\n"
    "  to build the bots correctly.")
endif()

file(GLOB_RECURSE sources_playerbot CONFIGURE_DEPENDS playerbot/*.cpp playerbot/*.h)
file(GLOB_RECURSE sources_ahbot CONFIGURE_DEPENDS ahbot/*.cpp ahbot/*.h)

add_library(plugins STATIC
  ${sources_playerbot}
  ${sources_ahbot})

target_include_directories(plugins
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/playerbot
    ${CMAKE_CURRENT_SOURCE_DIR}/ahbot
  PRIVATE
    ${CMAKE_BINARY_DIR})

target_link_libraries(plugins
  PUBLIC
    game-interface
    mysql)

# Modern TrinityCore uses flat includes across all core subdirectories.
set(PLUGIN_INCLUDES)
foreach(INCLUDE_ROOT
    ${CMAKE_SOURCE_DIR}/src/server/game
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/server/shared
    ${CMAKE_SOURCE_DIR}/src/server/database)
  CollectIncludeDirectories(${INCLUDE_ROOT} ROOT_INCLUDES)
  list(APPEND PLUGIN_INCLUDES ${ROOT_INCLUDES})
endforeach()

target_include_directories(plugins
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server
    # pchdef.h includes the game PCH (PrecompiledHeaders/gamePCH.h) instead of
    # repeating its contents; make sure that path always resolves even if the
    # recursive collection above is ever narrowed.
    ${CMAKE_SOURCE_DIR}/src/server/game/PrecompiledHeaders
    ${PLUGIN_INCLUDES})

add_definitions(-DTRINITY_API_EXPORT_plugins)

set_target_properties(plugins
  PROPERTIES
    FOLDER
      "server")

# Generate precompiled header.
# Every plugin translation unit already starts with #include "../pchdef.h",
# so turning it into a real PCH is free and cuts the compile time of the ~500
# bot sources down massively.
#
# pchdef.h holds the *core* headers (the ~74k lines every bot source used to
# re-parse); the playerbot headers themselves stay out of it on purpose so
# editing a bot header does not invalidate the PCH.
if(USE_COREPCH)
  add_cxx_pch(plugins ${CMAKE_CURRENT_SOURCE_DIR}/pchdef.h)
endif()

# ---------------------------------------------------------------------------
# Optional unity (jumbo) build for the plugin.
#
# The 258 bot sources are small (27k lines in total) but every one of them
# pulls in the whole core header set, so the per-file overhead dwarfs the
# actual compilation.  Merging them into batches of 8 translation units removes
# almost all of that overhead and stacks with the PCH above.
#
# This is OFF by default because translated sources share one translation unit
# and can clash (two files defining the same file-local helper, macros leaking
# into the next file, ...).  A scan of the tree shows the plugin is clean on
# MSVC - the known offenders below are excluded - but it is opt-in so a stock
# build never depends on it.
# ---------------------------------------------------------------------------
if(WITH_UNITY_BUILD)
  set_target_properties(plugins
    PROPERTIES
      UNITY_BUILD ON
      UNITY_BUILD_MODE BATCH
      UNITY_BUILD_BATCH_SIZE ${WITH_UNITY_BUILD_BATCH_SIZE})

  # These files define the same file-local helpers (strcmpi / strstri) and must
  # never be merged into the same translation unit.
  set_source_files_properties(
    playerbot/PlayerbotAI.cpp
    playerbot/GuildTaskMgr.cpp
    playerbot/RandomItemMgr.cpp
    playerbot/strategy/actions/WhoAction.cpp
    playerbot/strategy/values/ItemForSpellValue.cpp
    PROPERTIES
      SKIP_UNITY_BUILD_INCLUSION ON)

  message(STATUS "Playerbot plugin: unity build enabled (batch size ${WITH_UNITY_BUILD_BATCH_SIZE})")
endif()

# Playerbot settings are shipped in src/server/worldserver/worldserver.conf.dist
# and copied/installed by the worldserver target; no separate bot config is needed.
