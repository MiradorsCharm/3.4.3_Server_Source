# This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

cmake_minimum_required(VERSION 3.18)

# ---------------------------------------------------------------------------
# Windows-only build.
#
# Linux / macOS support has been removed from this tree on purpose: the server
# is shipped as Windows binaries, CI and the release pipeline build with MSVC
# on Windows only, and the half-maintained Unix build files were a constant
# source of breakage.  cmake/macros/CheckPlatform.cmake fails the configure
# step with a clear message on any other platform.
# ---------------------------------------------------------------------------

# add this options before PROJECT keyword
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

# Force the 64-bit hosted MSVC toolchain for Visual Studio generators.
#
# With the default x86 hosted cl.exe the compiler only has ~3GB of address
# space; building game/scripts (huge translation units + precompiled headers,
# several compilers in parallel through /MP) exhausts it and the build dies
# with:
#   C3859: Failed to create virtual memory for PCH
#   C1076: compiler limit: internal heap limit reached
# and finally LNK1181 for worldserver because scripts.lib was never produced.
#
# IMPORTANT: This must be set here (in CMakeLists.txt, before project()) and
# NOT in PreLoad.cmake with CACHE FORCE.  PreLoad.cmake runs before CMake
# loads the cache, so a CACHE FORCE write there creates the cache entry
# without updating CMake's internal toolset state.  On every subsequent
# configure the consistency check then sees:
#   internal toolset = ""  (no -T on the command line)
#   cached  toolset = "host=x64"  (written by PreLoad.cmake last time)
# and aborts with "generator toolset does not match the toolset used
# previously".  By setting the cache variable here, the cache has already been
# loaded, NOT CMAKE_GENERATOR_TOOLSET correctly reflects the cached value on
# re-runs, and the non-FORCE set() won't override an explicit user choice.
if(CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT CMAKE_GENERATOR_TOOLSET)
  set(CMAKE_GENERATOR_TOOLSET "host=x64" CACHE STRING "Platform Toolset")
endif()

# Set projectname (must be done AFTER setting configurationtypes)
project(TrinityCore)

# CMake policies (can not be handled elsewhere)
cmake_policy(SET CMP0005 NEW)
cmake_policy(SET CMP0043 NEW) # Ignore COMPILE_DEFINITIONS_<Config> properties
cmake_policy(SET CMP0054 NEW) # Only interpret if() arguments as variables or keywords when unquoted - prevents intepreting if(SOME_STRING_VARIABLE MATCHES "MSVC") as if(SOME_STRING_VARIABLE MATCHES "1")
cmake_policy(SET CMP0067 NEW) # Honor language standard in try_compile() source-file signature
cmake_policy(SET CMP0074 NEW) # find_package() uses <PackageName>_ROOT variables

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW) # find_package() uses upper-case <PACKAGENAME>_ROOT variables
endif()

if(POLICY CMP0153)
  cmake_policy(SET CMP0153 NEW) # The exec_program() command should not be called
endif()

# set macro-directory
list(APPEND CMAKE_MODULE_PATH
  "${CMAKE_SOURCE_DIR}/cmake/macros")

# build in Release-mode by default if not explicitly set
if(CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
  set(CMAKE_DEFAULT_BUILD_TYPE "RelWithDebInfo" CACHE INTERNAL "")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

include(CheckCXXSourceRuns)
include(CheckIncludeFiles)
include(ConfigureScripts)

# set default buildoptions and print them
include(cmake/options.cmake)

# Enable sccache / ccache when available (build speed, see docs/BuildPerformance.md).
# Has to happen before the first target is created.
include(${CMAKE_SOURCE_DIR}/cmake/macros/CompilerCache.cmake)
configure_compiler_cache()

# turn off PCH totally if enabled (hidden setting, mainly for devs)
if(NOPCH)
  set(USE_COREPCH 0)
  set(USE_SCRIPTPCH 0)
endif()

include(ConfigureBaseTargets)
include(CheckPlatform)

include(GroupSources)
include(AutoCollect)

find_package(PCHSupport)

if(NOT WITHOUT_GIT)
  find_package(Git 1.7)
endif()

# find mysql client binary (needed by genrev)
find_package(MySQL OPTIONAL_COMPONENTS binary)

# Find revision ID and hash of the sourcetree
include(cmake/genrev.cmake)

# print out the results before continuing
include(cmake/showoptions.cmake)

# add dependencies
add_subdirectory(dep)

# add core sources
add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
  list(APPEND CMAKE_MODULE_PATH
    "${Catch2_SOURCE_DIR}/contrib")
  include(Catch)

  add_subdirectory(tests)

  # Catch cmakefile messes with our settings we explicitly leave up to the user
  # restore user preference
  if (NOT WITH_SOURCE_TREE STREQUAL "hierarchical-folders")
    set_property(GLOBAL PROPERTY USE_FOLDERS OFF)
  endif()
endif()
