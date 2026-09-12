# This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as an exception the author gives
# unlimited permission to copy and/or distribute it, and to permit others to do
# the same, with or without modifications, as long as this notice is preserved.
#
# Enables sccache / ccache as a compiler launcher so that repeated builds
# (branch switching, clean rebuilds, CI) reuse previous object files.
#
# Must be a macro and not a function: CMAKE_<LANG>_COMPILER_LAUNCHER has to be
# set in the scope of the caller, otherwise the setting would be thrown away
# when the function returns.

macro(configure_compiler_cache)
  if(WITH_COMPILER_CACHE STREQUAL "none" OR NOT WITH_COMPILER_CACHE)
    message(STATUS "Compiler cache: disabled (-DWITH_COMPILER_CACHE=AUTO to enable)")
  else()
    if(WITH_COMPILER_CACHE STREQUAL "AUTO")
      # sccache first: it is the only one with solid MSVC support.
      set(_compiler_cache_candidates sccache ccache)
    else()
      set(_compiler_cache_candidates ${WITH_COMPILER_CACHE})
    endif()

    unset(TRINITY_COMPILER_CACHE CACHE)
    foreach(_compiler_cache_tool IN LISTS _compiler_cache_candidates)
      find_program(TRINITY_COMPILER_CACHE NAMES ${_compiler_cache_tool})
      if(TRINITY_COMPILER_CACHE)
        break()
      endif()
    endforeach()

    if(TRINITY_COMPILER_CACHE)
      set(CMAKE_C_COMPILER_LAUNCHER "${TRINITY_COMPILER_CACHE}")
      set(CMAKE_CXX_COMPILER_LAUNCHER "${TRINITY_COMPILER_CACHE}")

      get_filename_component(_compiler_cache_name "${TRINITY_COMPILER_CACHE}" NAME_WE)
      message(STATUS "Compiler cache: ${_compiler_cache_name} (${TRINITY_COMPILER_CACHE})")

      if(_compiler_cache_name STREQUAL "ccache" AND MSVC)
        message(WARNING
          "  ccache's MSVC support cannot cache /Zi builds safely.\n"
          "  Either configure with -DWITH_FAST_DEBUGINFO=1 (uses /Z7) or install\n"
          "  sccache instead, which handles MSVC program databases correctly.")
      endif()

      if(_compiler_cache_name STREQUAL "ccache")
        message(STATUS "Compiler cache: remember to raise the cache size for this tree, e.g. 'ccache --set-config max_size=20G'")
      else()
        message(STATUS "Compiler cache: remember to raise the cache size for this tree, e.g. 'setx SCCACHE_CACHE_SIZE 20G'")
      endif()
    else()
      message(STATUS "Compiler cache: ${WITH_COMPILER_CACHE} requested but no such program was found")
    endif()
  endif()
endmacro()
