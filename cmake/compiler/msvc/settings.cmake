# set up output paths for executable binaries (.exe-files, and .dll-files on DLL-capable platforms)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(MSVC_EXPECTED_VERSION 19.32)
set(MSVC_EXPECTED_VERSION_STRING "Microsoft Visual Studio 2022 17.2")

# This file is also used by compilers that pretend to be MSVC but report their own version number - don't version check them
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS MSVC_EXPECTED_VERSION)
    message(FATAL_ERROR "MSVC: TrinityCore requires version ${MSVC_EXPECTED_VERSION} (${MSVC_EXPECTED_VERSION_STRING}) to build but found ${CMAKE_CXX_COMPILER_VERSION}")
  else()
    message(STATUS "MSVC: Minimum version required is ${MSVC_EXPECTED_VERSION}, found ${CMAKE_CXX_COMPILER_VERSION} - ok!")
  endif()
endif()

# CMake sets warning flags by default, however we manage it manually
# for different core and dependency targets
string(REGEX REPLACE "/W[0-4] " "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
# Search twice, once for space after /W argument,
# once for end of line as CMake regex has no \b
string(REGEX REPLACE "/W[0-4]$" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "/W[0-4] " "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REGEX REPLACE "/W[0-4]$" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

target_compile_options(trinity-warning-interface
  INTERFACE
    /W3)

# disable permissive mode to make msvc more eager to reject code that other compilers don't already accept
target_compile_options(trinity-compile-option-interface
  INTERFACE
    /permissive-)

# set up output paths ofr static libraries etc (commented out - shown here as an example only)
#set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(PLATFORM EQUAL 64)
  # This definition is necessary to work around a bug with Intellisense described
  # here: http://tinyurl.com/2cb428.  Syntax highlighting is important for proper
  # debugger functionality.
  target_compile_definitions(trinity-compile-option-interface
    INTERFACE
      -D_WIN64)

  message(STATUS "MSVC: 64-bit platform, enforced -D_WIN64 parameter")

else()
  # mark 32 bit executables large address aware so they can use > 2GB address space
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /LARGEADDRESSAWARE")
  message(STATUS "MSVC: Enabled large address awareness")

  target_compile_options(trinity-compile-option-interface
    INTERFACE
      /arch:SSE2)
  message(STATUS "MSVC: Enabled SSE2 support")

  set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} /SAFESEH:NO")
  message(STATUS "MSVC: Disabled Safe Exception Handlers for debug builds")
endif()

# Set build-directive (used in core to tell which buildtype we used)
# msbuild/devenv don't set CMAKE_MAKE_PROGRAM, you can choose build type from a dropdown after generating projects
if("${CMAKE_MAKE_PROGRAM}" MATCHES "MSBuild")
  target_compile_definitions(trinity-compile-option-interface
    INTERFACE
      -D_BUILD_DIRECTIVE="$(ConfigurationName)")
else()
  # while all make-like generators do (nmake, ninja)
  target_compile_definitions(trinity-compile-option-interface
    INTERFACE
      -D_BUILD_DIRECTIVE="$<CONFIG>")
endif()

# multithreaded compiling on VS
#
# /MP makes cl.exe spawn its own worker processes for the translation units of
# one project.  That is exactly what the Visual Studio / MSBuild generator
# needs (MSBuild only parallelises across projects).
#
# With Ninja it is actively harmful: Ninja already keeps one cl.exe per core
# busy, and every one of them would then fan out again with /MP, which only
# oversubscribes the machine and thrashes memory.  Ninja builds are faster
# without it.
if(CMAKE_GENERATOR MATCHES "Ninja")
  message(STATUS "MSVC: Ninja generator - /MP omitted (Ninja already drives one cl.exe per core)")
else()
  target_compile_options(trinity-compile-option-interface
    INTERFACE
      /MP)
endif()

# Make sure the 64-bit hosted compiler is used.
# The x86 hosted cl.exe is limited to a ~3GB address space and fails to build
# this tree with "C3859: Failed to create virtual memory for PCH" followed by
# "C1076: compiler limit: internal heap limit reached", which in turn makes the
# scripts/game libraries missing and worldserver fail with LNK1181.
if(CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE AND
   NOT CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE MATCHES "^(x64|X64)$")
  message(WARNING "MSVC: 32-bit hosted toolset selected (host=${CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE}). "
                  "This build requires the 64-bit hosted compiler - configure with -T host=x64.")
endif()


if((PLATFORM EQUAL 64) OR (NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.0.23026.0) OR BUILD_SHARED_LIBS)
  # Enable extended object support
  target_compile_options(trinity-compile-option-interface
    INTERFACE
      /bigobj)

  message(STATUS "MSVC: Enabled increased number of sections in object files")
endif()

# /Zc:throwingNew.
# When you specify Zc:throwingNew on the command line, it instructs the compiler to assume
# that the program will eventually be linked with a conforming operator new implementation,
# and can omit all of these extra null checks from your program.
# http://blogs.msdn.com/b/vcblog/archive/2015/08/06/new-in-vs-2015-zc-throwingnew.aspx
target_compile_options(trinity-compile-option-interface
  INTERFACE
    /Zc:throwingNew)

# Define _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES - eliminates the warning by changing the strcpy call to strcpy_s, which prevents buffer overruns
target_compile_definitions(trinity-compile-option-interface
  INTERFACE
    -D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES)
message(STATUS "MSVC: Overloaded standard names")

# Ignore warnings about older, less secure functions
target_compile_definitions(trinity-compile-option-interface
  INTERFACE
    -D_CRT_SECURE_NO_WARNINGS)
message(STATUS "MSVC: Disabled NON-SECURE warnings")

# Ignore warnings about POSIX deprecation
target_compile_definitions(trinity-compile-option-interface
  INTERFACE
    -D_CRT_NONSTDC_NO_WARNINGS)

message(STATUS "MSVC: Disabled POSIX warnings")

# Ignore specific warnings
target_compile_options(trinity-compile-option-interface
  INTERFACE
    /wd4351  # C4351: new behavior: elements of array 'x' will be default initialized
    /wd4091) # C4091: 'typedef ': ignored on left of '' when no variable is declared

if(NOT WITH_WARNINGS)
  target_compile_options(trinity-compile-option-interface
    INTERFACE
      /wd4996  # C4996 deprecation
      /wd4985  # C4985 'symbol-name': attributes not present on previous declaration.
      /wd4244  # C4244 'argument' : conversion from 'type1' to 'type2', possible loss of data
      /wd4267  # C4267 'var' : conversion from 'size_t' to 'type', possible loss of data
      /wd4619  # C4619 #pragma warning : there is no warning number 'number'
      /wd4512) # C4512 'class' : assignment operator could not be generated

  message(STATUS "MSVC: Disabled generic compiletime warnings")
endif()

if(BUILD_SHARED_LIBS)
  target_compile_options(trinity-compile-option-interface
    INTERFACE
      /wd4251  # C4251: needs to have dll-interface to be used by clients of class '...'
      /wd4275) # C4275: non dll-interface class ...' used as base for dll-interface class '...'

  message(STATUS "MSVC: Enabled shared linking")
endif()

# Move some warnings that are enabled for other compilers from level 4 to level 3
target_compile_options(trinity-compile-option-interface
  INTERFACE
    /w34100  # C4100 'identifier' : unreferenced formal parameter
    /w34101  # C4101: 'identifier' : unreferenced local variable
    /w34189  # C4189: 'identifier' : local variable is initialized but not referenced
    /w34389  # C4389: 'equality-operator' : signed/unsigned mismatch
	/w35054) # C5054: 'operator 'operator-name': deprecated between enumerations of different types'

# Enable and treat as errors the following warnings to easily detect virtual function signature failures:
# 'function' : member function does not override any base class virtual member function
# 'virtual_function' : no override available for virtual member function from base 'class'; function is hidden
target_compile_options(trinity-compile-option-interface
  INTERFACE
    /we4263
    /we4264)

if(ASAN)
  target_compile_definitions(trinity-compile-option-interface
    INTERFACE
      -D_DISABLE_STRING_ANNOTATION
      -D_DISABLE_VECTOR_ANNOTATION)

  target_compile_options(trinity-compile-option-interface
    INTERFACE
      /fsanitize=address)

  message(STATUS "MSVC: Enabled Address Sanitizer ASan")
endif()

# Disable incremental linking in debug builds.
# To prevent linking getting stuck (which might be fixed in a later VS version).
macro(DisableIncrementalLinking variable)
  string(REGEX REPLACE "/INCREMENTAL *" "" ${variable} "${${variable}}")
  set(${variable} "${${variable}} /INCREMENTAL:NO")
endmacro()

# Disable Visual Studio 2022 build process management
# This will make compiler behave like in 2019 - compiling num_cpus * num_projects at the same time
# it is neccessary because of a bug in current implementation that makes scripts build only a single
# file at the same time after game project finishes building
file(COPY "${CMAKE_CURRENT_LIST_DIR}/Directory.Build.props" DESTINATION "${CMAKE_BINARY_DIR}")

DisableIncrementalLinking(CMAKE_EXE_LINKER_FLAGS_DEBUG)
DisableIncrementalLinking(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO)
DisableIncrementalLinking(CMAKE_SHARED_LINKER_FLAGS_DEBUG)
DisableIncrementalLinking(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO)

# ---------------------------------------------------------------------------
# Build speed switches (see docs/BuildPerformance.md).
#
# Both are off by default - they trade disk space / debugger convenience for
# wall-clock time - but they are one -D away and are what Build-Fast.ps1 uses.
# ---------------------------------------------------------------------------

# /Zi makes every cl.exe process funnel its debug info through the shared
# mspdbsrv.exe service, which serialises the whole build and is a classic
# source of C1902 / stuck builds.  /Z7 embeds the debug info in the object
# files instead: fully parallel, at the cost of much larger .obj files and a
# somewhat slower link.  The linker still produces the normal PDB thanks to
# /DEBUG, so debugging is unaffected.
if(WITH_FAST_DEBUGINFO)
  foreach(_lang C CXX)
    foreach(_config DEBUG RELWITHDEBINFO)
      string(REGEX REPLACE "/[Zz][iI]" "/Z7" CMAKE_${_lang}_FLAGS_${_config} "${CMAKE_${_lang}_FLAGS_${_config}}")
    endforeach()
  endforeach()
  message(STATUS "MSVC: using /Z7 debug info (WITH_FAST_DEBUGINFO) - faster parallel compiles, larger objects")
endif()

# /DEBUG:FASTLINK keeps type information in the object files and only builds a
# partial PDB, which makes linking worldserver dramatically faster.  Debuggers
# then resolve types on demand (slightly slower first lookup).
if(WITH_FASTLINK)
  foreach(_config DEBUG RELWITHDEBINFO)
    foreach(_var CMAKE_EXE_LINKER_FLAGS_${_config} CMAKE_SHARED_LINKER_FLAGS_${_config} CMAKE_MODULE_LINKER_FLAGS_${_config})
      if(${_var} MATCHES "/[Dd][Ee][Bb][Uu][Gg]")
        string(REGEX REPLACE "/[Dd][Ee][Bb][Uu][Gg](:[A-Za-z]+)?" "/DEBUG:FASTLINK" ${_var} "${${_var}}")
      else()
        set(${_var} "${${_var}} /DEBUG:FASTLINK")
      endif()
    endforeach()
  endforeach()
  message(STATUS "MSVC: linking with /DEBUG:FASTLINK (WITH_FASTLINK)")
endif()
