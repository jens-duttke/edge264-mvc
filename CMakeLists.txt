# ==============================================================================
# edge264-mvc - CMakeLists.txt
# Generated with AI assistance by Claude Sonnet 4.6 (claude-sonnet-4-6).
#
# Thin wrapper that delegates all build logic to the Makefile.
# It maps standard CMake variables to Makefile parameters and exposes an
# imported target edge264mvc::edge264mvc for use with target_link_libraries.
#
# The only value to keep in sync with the Makefile when it changes is
# EDGE264MVC_MAJOR below.
# ==============================================================================

cmake_minimum_required(VERSION 3.14)
project(edge264mvc VERSION 2.0 LANGUAGES C)

# Keep in sync with MAJOR in the Makefile.
set(EDGE264MVC_MAJOR 2)

# Optional: forward a VARIANTS string to the Makefile
# (e.g. -DEDGE264MVC_VARIANTS=x86-64-v2,x86-64-v3,x86-64-v4,logs).
set(EDGE264MVC_VARIANTS "" CACHE STRING
    "Comma-separated edge264-mvc build variants (x86-64-v2, x86-64-v3, x86-64-v4, logs)")

include(ExternalProject)

# ---- Map CMAKE_SYSTEM_NAME to Makefile OS values ----------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(_os macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(_os ios)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(_os windows)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(_os android)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(_os wasm)
else()
  set(_os linux)
endif()

# ---- Static vs shared -------------------------------------------------------
# iOS is always static regardless of BUILD_SHARED_LIBS.
if(_os STREQUAL "ios" OR NOT BUILD_SHARED_LIBS)
  set(_static yes)
else()
  set(_static no)
endif()

# ---- Expected library filename -----------------------------------------------
# Must match the LIBNAME logic in the Makefile.
if(_static STREQUAL "yes")
  set(_libfile libedge264mvc.a)
elseif(_os STREQUAL "macos")
  set(_libfile libedge264mvc.${EDGE264MVC_MAJOR}.dylib)
elseif(_os STREQUAL "linux")
  set(_libfile libedge264mvc.so.${EDGE264MVC_MAJOR})
elseif(_os STREQUAL "windows")
  set(_libfile edge264mvc.${EDGE264MVC_MAJOR}.dll)
elseif(_os STREQUAL "android")
  set(_libfile libedge264mvc.so)
elseif(_os STREQUAL "wasm")
  set(_libfile edge264mvc.js)
endif()

# ---- Installation prefix inside the CMake build tree ------------------------
set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/edge264mvc-install")

# ---- Makefile arguments -----------------------------------------------------
set(_args
  OS=${_os}
  STATIC=${_static}
  BUILDTEST=no
  PREFIX=${_prefix}
)

if(CMAKE_C_COMPILER)
  list(APPEND _args CC=${CMAKE_C_COMPILER})
endif()

# Forward CMAKE_C_FLAGS only when non-empty; an empty CFLAGS= on the command
# line would override the Makefile's own default flags.
if(CMAKE_C_FLAGS)
  list(APPEND _args "CFLAGS=${CMAKE_C_FLAGS}")
endif()

if(EDGE264MVC_VARIANTS)
  list(APPEND _args VARIANTS=${EDGE264MVC_VARIANTS})
endif()

# Android: use the NDK's llvm-ar co-located with the compiler to avoid a
# host/target mismatch when archiving (see Makefile for details).
if(_os STREQUAL "android" AND CMAKE_C_COMPILER)
  get_filename_component(_ndk_bin "${CMAKE_C_COMPILER}" DIRECTORY)
  list(APPEND _args AR=${_ndk_bin}/llvm-ar)
endif()

# ---- ExternalProject ---------------------------------------------------------
ExternalProject_Add(edge264mvc_ext
  SOURCE_DIR        "${CMAKE_CURRENT_SOURCE_DIR}"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     $(MAKE) ${_args}
  INSTALL_COMMAND   $(MAKE) ${_args} install
  # Objects are written next to the sources; do not copy the tree.
  BUILD_IN_SOURCE   TRUE
  # Declaring the output lets CMake track the dependency correctly.
  BUILD_BYPRODUCTS  "${_prefix}/lib/${_libfile}" "${_prefix}/lib/libedge264mvc.dll.a"
)

# ---- Imported target ---------------------------------------------------------
if(_static STREQUAL "yes")
  add_library(edge264mvc STATIC IMPORTED GLOBAL)
else()
  add_library(edge264mvc SHARED IMPORTED GLOBAL)
endif()

add_dependencies(edge264mvc edge264mvc_ext)
# The include directory is created by the install step at build time, but
# CMake rejects an imported target whose include directory does not exist
# yet when it configures the consumer, so create it now.
file(MAKE_DIRECTORY "${_prefix}/include")
set_target_properties(edge264mvc PROPERTIES
  IMPORTED_LOCATION             "${_prefix}/lib/${_libfile}"
  INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include")
if(_os STREQUAL "windows" AND _static STREQUAL "no")
  # a shared library is linked through its import library on Windows
  set_target_properties(edge264mvc PROPERTIES IMPORTED_IMPLIB "${_prefix}/lib/libedge264mvc.dll.a")
endif()

# Canonical namespaced alias consumed by downstream targets.
add_library(edge264mvc::edge264mvc ALIAS edge264mvc)
