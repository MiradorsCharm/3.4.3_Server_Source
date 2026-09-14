#
# Playerbot AI plugin.
#
# The playerbot AI is a from-scratch implementation for this 3.4.3 core: bots
# are socket-less players whose movement, facing and combat are driven through
# the same core pipelines a real client drives (see docs/Playerbots.md).
#
# IMPORTANT: The playerbot plugin calls core functions that are not marked
# TC_GAME_API. When WITH_DYNAMIC_LINKING is ON the core libraries use
# -fvisibility=hidden and those symbols become unavailable; the plugin
# therefore requires static linking (WITH_DYNAMIC_LINKING=OFF).

if(BUILD_SHARED_LIBS)
  message(WARNING
    "  Playerbot plugin: BUILD_SHARED_LIBS is ON (WITH_DYNAMIC_LINKING=1).\n"
    "  The bot code calls core functions that lack TC_GAME_API exports,\n"
    "  so the build will almost certainly fail to link. Reconfigure with\n"
    "    -DWITH_DYNAMIC_LINKING=0\n"
    "  to build the bots correctly.")
endif()

file(GLOB sources_playerbot CONFIGURE_DEPENDS playerbot/*.cpp playerbot/*.h)

add_library(plugins STATIC
  ${sources_playerbot})

target_include_directories(plugins
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/playerbot
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
    ${CMAKE_SOURCE_DIR}/src/server/game/PrecompiledHeaders
    ${PLUGIN_INCLUDES})

add_definitions(-DTRINITY_API_EXPORT_plugins)

set_target_properties(plugins
  PROPERTIES
    FOLDER
      "server")

# Generate the precompiled header. Every plugin translation unit starts with
# #include "pchdef.h" (the core header set), which the build precompiles.
if(USE_COREPCH)
  add_cxx_pch(plugins ${CMAKE_CURRENT_SOURCE_DIR}/pchdef.h)
endif()

# Playerbot settings are shipped in src/server/worldserver/worldserver.conf.dist
# and copied/installed by the worldserver target; no separate config is needed.
