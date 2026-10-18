# ==============================================================================
# edge264-mvc - Makefile
# Generated with AI assistance by Claude Sonnet 4.6 (claude-sonnet-4-6).
#
# Supported targets: macOS (macos), Linux (linux), Windows MinGW (windows),
#                    WebAssembly (wasm), Android NDK (android), iOS (ios)
#
# Optional parameters (all can be overridden on the command line):
#
#   CC          - compiler used for object file compilation
#                 (auto-detected by target OS)
#   CCLD        - compiler driver used for the final link step (default: CC)
#                 Set this when the compiler and linker must differ, e.g. when
#                 cross-compiling objects with Clang (--target=…) but linking
#                 with a target-prefixed GCC (aarch64-linux-gnu-gcc) to pick up
#                 the correct crt0.o and libgcc from the target sysroot.
#   AR          - archiver used when STATIC=yes (default: ar; for Android NDK
#                 cross-compilation use the NDK's llvm-ar to avoid host/target
#                 mismatch, e.g. AR=$NDK/.../llvm-ar)
#   OS          - target operating system (default: host OS)
#                 accepted values: macos  linux  windows  wasm  android  ios
#   VARIANTS    - comma-separated build variants, from:
#                   x86-64-v2  → build an extra edge264mvc_headers object with
#                                -march=x86-64-v2 for runtime dispatch (SSE4.1)
#                   x86-64-v3  → same with -march=x86-64-v3 (AVX2)
#                   x86-64-v4  → same with -march=x86-64-v4 (AVX-512)
#                   logs       → build the debug/logging variant
#                 x86-64-v2/v3/v4 are intended for distribution packages that must
#                 run efficiently across a wide range of x86 CPUs: the library
#                 detects the host ISA level at runtime and dispatches to the
#                 fastest available implementation.  They are NOT needed for a
#                 native single-machine build, where -march=native already picks
#                 the best code path at compile time.  (default: logs)
#   SANITIZE    - comma-separated sanitizer list passed directly to -fsanitize=,
#                 e.g. SANITIZE=address,undefined  (default: empty)
#   STATIC      - produce a static library (.a) instead of shared: yes|no
#                 (default: no; always forced to yes for iOS)
#   PREFIX      - installation prefix (default: /usr/local)
#   libdir      - library installation directory (default: $(PREFIX)/lib)
#   includedir  - header installation directory (default: $(PREFIX)/include)
#   DESTDIR     - staging root for package managers (default: empty)
#   CPPFLAGS    - extra preprocessor flags on C source files
#   CFLAGS      - extra compiler flags on C source files
#   LDFLAGS     - extra linker flags passed to every link invocation
#   OBJFLAGS    - extra flags passed when generating object files
#   LIBFLAGS    - extra flags passed when generating library files
#   EXEFLAGS    - extra flags passed when generating executable files
#   BUILDTEST   - build the test executable: yes|no (default: yes)
#   V           - verbose build output: yes|no (default: yes)
#   PY          - Python interpreter (default: python3)
#
# Cross-compilation note:
#   SSE/AVX and NEON intrinsics are enabled automatically by the compiler when
#   the target ISA is specified via -march (e.g. -march=x86-64-v3 or
#   -march=armv8-a+simd) or -arch arm64 (Apple clang).  Always set an explicit
#   -march in CFLAGS when cross-compiling.
# ==============================================================================

# If a recipe fails mid-way, delete the partially-written target so the next
# invocation does not mistake it for an up-to-date file.
.DELETE_ON_ERROR:

# ---- Version -----------------------------------------------------------------
MAJOR   := 2
MINOR   := 0
VERSION := $(MAJOR).$(MINOR)
# the version reported by edge264mvc_version(): the release tag when built from git
GIT_VERSION := $(shell git describe --tags --always --dirty 2>/dev/null)
ifneq ($(GIT_VERSION),)
  override CPPFLAGS += -DEDGE264MVC_VERSION_STRING='"$(GIT_VERSION)"'
endif

# ---- Host OS detection -------------------------------------------------------
# uname -s returns: Linux, Darwin, MINGW64_NT-*, MSYS_NT-*, CYGWIN_NT-* ...
# On Windows cmd without uname the fallback is "windows".
# Everything is lowercased and normalized to the accepted OS values.
_UNAME    := $(shell uname -s 2>/dev/null || echo windows)
_UNAME_LC := $(shell echo $(_UNAME) | tr '[:upper:]' '[:lower:]')
HOST_OS   := $(strip \
               $(if $(findstring environment,$(origin EMSCRIPTEN)),wasm,\
               $(if $(findstring mingw,$(_UNAME_LC)),windows,\
               $(if $(findstring msys,$(_UNAME_LC)),windows,\
               $(if $(findstring cygwin,$(_UNAME_LC)),windows,\
               $(if $(findstring darwin,$(_UNAME_LC)),macos,\
                 $(_UNAME_LC)))))))

# ---- Target OS ---------------------------------------------------------------
# Defaults to the host; can be overridden for cross-compilation.
OS ?= $(HOST_OS)
ifeq (,$(findstring $(OS),macos linux windows wasm android ios))
  $(error OS=$(OS) is invalid. Accepted values: macos  linux  windows  wasm  android  ios)
endif

# ---- Compiler / linker selection ---------------------------------------------
# CC is used for all object file compilation.
ifeq ($(OS),ios)
  # xcrun selects the right clang from the active Xcode installation
  CC ?= $(shell xcrun --sdk iphoneos --find clang 2>/dev/null || echo clang)
else ifneq ($(OS),wasm)
  CC ?= cc
endif

# CCLD is used for linking (defaults to CC); set it explicitly when the compiler
# and linker driver must differ, e.g. compiling with Clang (--target=…) while
# linking with a target-prefixed GCC (aarch64-linux-gnu-gcc) to pick up the
# correct crt0.o and libgcc from the target sysroot.
CCLD ?= $(CC)

# AR is used for static builds. Android NDK ships llvm-ar alongside its clang;
# using the host ar against NDK objects can silently produce a corrupt archive.
ifeq ($(OS),android)
  AR ?= llvm-ar
endif

# ---- User parameters ---------------------------------------------------------
VARIANTS   ?= logs
SANITIZE   ?=
STATIC     ?= no
PREFIX     ?= /usr/local
libdir     ?= $(PREFIX)/lib
includedir ?= $(PREFIX)/include
DESTDIR    ?=
BUILDTEST  ?= yes
V          ?= yes
PY         ?= python3

# iOS is always a static build
ifeq ($(OS),ios)
  override STATIC := yes
endif

# ---- VARIANTS parsing --------------------------------------------------------
HAS_V2   := $(findstring x86-64-v2,$(VARIANTS))
HAS_V3   := $(findstring x86-64-v3,$(VARIANTS))
HAS_V4   := $(findstring x86-64-v4,$(VARIANTS))
HAS_LOGS := $(findstring logs,$(VARIANTS))

# x86 variants are meaningless on non-x86 targets
ifneq (,$(findstring $(OS),wasm android ios))
  ifneq ($(HAS_V2)$(HAS_V3)$(HAS_V4),)
    $(warning WARNING: x86-64-v2/v3/v4 variants are ignored for OS=$(OS))
    HAS_V2 :=
    HAS_V3 :=
    HAS_V4 :=
  endif
endif

# ---- Object file list --------------------------------------------------------
OBJNAMES := edge264mvc.o \
  $(if $(HAS_V2),edge264mvc_headers_v2.o) \
  $(if $(HAS_V3),edge264mvc_headers_v3.o) \
  $(if $(HAS_V4),edge264mvc_headers_v4.o) \
  $(if $(HAS_LOGS),edge264mvc_headers_log.o)

# ---- Output filenames per target ---------------------------------------------
ifeq ($(OS),macos)
  LIBNAME := libedge264mvc.$(MAJOR).dylib
else ifeq ($(OS),linux)
  LIBNAME := libedge264mvc.so.$(MAJOR)
else ifeq ($(OS),windows)
  LIBNAME := edge264mvc.$(MAJOR).dll
  EXE := .exe
else ifeq ($(OS),android)
  # Android does not support versioned .so filenames
  LIBNAME := libedge264mvc.so
else ifeq ($(OS),ios)
  # iOS requires static libraries or signed .xcframework bundles
  LIBNAME := libedge264mvc.a
else ifeq ($(OS),wasm)
  # emcc produces a .js glue file alongside a .wasm binary
  LIBNAME := edge264mvc.js
  EXE := .js
endif

# Static builds override the shared library name
ifeq ($(STATIC),yes)
  LIBNAME := libedge264mvc.a
endif

# ---- Sanitizer flags ---------------------------------------------------------
# SANITIZE is passed verbatim to -fsanitize=, e.g. SANITIZE=address,undefined.
# Extra flags are appended for well-known sanitizers when detected.
ifneq ($(SANITIZE),)
  SANITIZE_FLAGS := -fsanitize=$(SANITIZE)
  ifneq (,$(findstring address,$(SANITIZE)))
    # AddressSanitizer: improve stack trace readability
    SANITIZE_FLAGS += -fno-omit-frame-pointer -g
    ifneq (,$(findstring memory,$(SANITIZE)))
      $(error SANITIZE: 'address' and 'memory' are mutually exclusive)
    endif
  endif
  ifneq (,$(findstring memory,$(SANITIZE)))
    # MemorySanitizer: requires -fPIE for reliable interception
    SANITIZE_FLAGS += -fPIE
  endif
  ifneq (,$(findstring undefined,$(SANITIZE)))
    # UndefinedBehaviorSanitizer: every finding fails the run
    SANITIZE_FLAGS += -fno-sanitize-recover=undefined
  endif
endif

# ---- Base architecture flags -------------------------------------------------
# -march=native is only injected for native builds (OS == HOST_OS), paired with
# -mtune=generic: for this hand-written-SIMD codebase GCC's per-microarch cost
# model (e.g. -mtune=znver4, implied by -march=native) schedules measurably slower
# code than generic tuning (~5-6% single-thread on Zen4/GCC), while -march=native
# still selects the host ISA. For cross-compilation the user is responsible for
# providing an explicit -march (or -arch on Apple clang) via CFLAGS.
ifeq ($(OS),macos)
  _BASE_ARCH := $(if $(findstring $(OS),$(HOST_OS)),-march=native -mtune=generic)
else ifeq ($(OS),linux)
  _BASE_ARCH := $(if $(findstring $(OS),$(HOST_OS)),-march=native -mtune=generic)
else ifeq ($(OS),windows)
  _BASE_ARCH := $(if $(findstring $(OS),$(HOST_OS)),-march=native -mtune=generic)
else ifeq ($(OS),android)
  # The NDK clang already targets the right architecture via its triple;
  # -march=native would describe the host CPU, not the Android device.
  _BASE_ARCH :=
else ifeq ($(OS),ios)
  # arm64 is the only live iOS architecture; xcrun resolves the sysroot.
  _IOS_SDK   := $(shell xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)
  _BASE_ARCH := -arch arm64 $(if $(_IOS_SDK),-isysroot $(_IOS_SDK))
else ifeq ($(OS),wasm)
  # add CFLAGS=-mrelaxed-simd for WASM v3 (requires runtime support)
  _BASE_ARCH := -msimd128
endif

# ---- Final CFLAGS ------------------------------------------------------------
# Required flags are prepended; user CFLAGS come last so they can always override.
# -fwrapv: the SIMD code relies on vector lanes wrapping on overflow (deblocking,
# transforms, CABAC contexts), which GCC otherwise treats as undefined.
_THREAD_FLAG := $(if $(findstring $(OS),macos linux android windows),-pthread)
override CFLAGS := $(_BASE_ARCH) -std=gnu11 -O3 -fwrapv -flax-vector-conversions -Wno-override-init $(_THREAD_FLAG) $(SANITIZE_FLAGS) $(CFLAGS)

# ---- Object file flags -------------------------------------------------------
# -fPIC is required for shared libraries on ELF targets.
# Not needed for static builds, Windows DLLs, or WASM.
# Only the edge264mvc_* API leaves the shared library: everything else is
# hidden, so internal names cannot clash with other libraries in a process.
# The static library marks nothing for export, so a library or DLL that links
# it in does not export the decoder API itself.
ifeq ($(STATIC),no)
  _PIC_FLAG := $(if $(findstring $(OS),macos linux android),-fPIC)
  _EXPORT_FLAG := -DEDGE264MVC_BUILD
endif
override OBJFLAGS := $(_PIC_FLAG) -fvisibility=hidden $(_EXPORT_FLAG) $(OBJFLAGS)

# ---- Common linker flags -----------------------------------------------------
ifeq ($(OS),wasm)
  override LDFLAGS := -sSTRICT=1 -sALLOW_MEMORY_GROWTH=1 $(SANITIZE_FLAGS) $(LDFLAGS)
else ifeq ($(OS),windows)
  # link winpthreads and the GCC runtime statically, so that the DLL and the
  # tools need no MinGW runtime DLL beside them
  override LDFLAGS := -static $(SANITIZE_FLAGS) $(LDFLAGS)
else
  override LDFLAGS := $(SANITIZE_FLAGS) $(LDFLAGS)
endif

# ---- Linker flags for the dynamic library ------------------------------------
ifeq ($(OS),macos)
  # -install_name @rpath lets the test binary find the dylib without DYLD_LIBRARY_PATH
  override LIBFLAGS := -shared -dynamiclib -install_name @rpath/$(LIBNAME) $(LDFLAGS) $(LIBFLAGS)
else ifeq ($(OS),linux)
  override LIBFLAGS := -shared -Wl,-soname,libedge264mvc.so.$(MAJOR) $(LDFLAGS) $(LIBFLAGS)
else ifeq ($(OS),android)
  override LIBFLAGS := -shared $(LDFLAGS) $(LIBFLAGS)
else ifeq ($(OS),windows)
  # the import library lets MinGW link with -ledge264mvc (pkg-config) and gives
  # CMake the IMPORTED_IMPLIB a shared library needs on Windows
  override LIBFLAGS := -shared -Wl,--out-implib,libedge264mvc.dll.a $(_THREAD_FLAG) $(LDFLAGS) $(LIBFLAGS)
else ifeq ($(OS),wasm)
  override LIBFLAGS := -sEXPORTED_FUNCTIONS=_malloc,_free,_edge264mvc_api_version,_edge264mvc_version,_edge264mvc_default_settings,_edge264mvc_open,_edge264mvc_close,_edge264mvc_send_nal,_edge264mvc_send_end,_edge264mvc_receive_frame,_edge264mvc_release_frame,_edge264mvc_flush,_edge264mvc_find_start_code $(LDFLAGS) $(LIBFLAGS)
endif

# ---- Linker flags for the executables ----------------------------------------
ifeq ($(OS),linux)
  ifeq ($(STATIC),yes)
    override EXEFLAGS := $(LIBNAME) $(LDFLAGS) $(EXEFLAGS)
  else
    # RPATH=. allows the test binary to find the .so from its own directory
    override EXEFLAGS := -Wl,-rpath,'$$ORIGIN' $(LIBNAME) $(LDFLAGS) $(EXEFLAGS)
  endif
else ifeq ($(OS),macos)
  ifeq ($(STATIC),yes)
    override EXEFLAGS := $(LIBNAME) $(LDFLAGS) $(EXEFLAGS)
  else
    # @loader_path resolves relative to the executable's own directory,
    # equivalent to Linux's $ORIGIN; required because the dylib is built with
    # -install_name @rpath/$(LIBNAME).
    override EXEFLAGS := -Wl,-rpath,@loader_path $(LIBNAME) $(LDFLAGS) $(EXEFLAGS)
  endif
else ifeq ($(OS),wasm)
  override EXEFLAGS := -sNODERAWFS=1 $(OBJNAMES) $(LDFLAGS) $(EXEFLAGS)
else
  override EXEFLAGS := $(LIBNAME) $(LDFLAGS) $(EXEFLAGS)
endif

# ---- Runtime dispatch defines ------------------------------------------------
RUNTIME_TESTS := \
  $(if $(HAS_V2),-DHAS_X86_64_V2) \
  $(if $(HAS_V3),-DHAS_X86_64_V3) \
  $(if $(HAS_V4),-DHAS_X86_64_V4) \
  $(if $(HAS_LOGS),-DHAS_LOGS)

# ---- Test files --------------------------------------------------------------
TESTS_YAML := $(wildcard tests/*.yaml)
TESTS_264  := $(patsubst %.yaml,%.264,$(TESTS_YAML))

# ---- Verbose mode ------------------------------------------------------------
Q = $(if $(findstring yes,$(V)),,@)

# ---- Compiler existence check ------------------------------------------------
# Only enforced for native builds; for cross-compilation the compiler binary
# may live under an absolute path not on the host PATH.
ifeq ($(OS),$(HOST_OS))
  _WHICH := $(if $(findstring windows,$(HOST_OS)),where 2>nul,which 2>/dev/null)
  ifeq (,$(shell $(_WHICH) $(CC) 2>/dev/null))
    $(error CC=$(CC) not found. Install gcc or clang, or pass CC=<path>)
  endif
endif


# ==============================================================================
# Build rules
# ==============================================================================

.PHONY: all
all: $(LIBNAME) $(if $(findstring yes,$(BUILDTEST)),edge264mvc_test$(EXE))

# ---- Library -----------------------------------------------------------------
# FIXME remove test here
ifeq ($(STATIC),yes)
$(LIBNAME): $(OBJNAMES)
	$(Q)$(AR) rcs $@ $^
else
$(LIBNAME): $(OBJNAMES)
	$(Q)$(CCLD) $^ $(LIBFLAGS) -o $@
endif

# ---- Test executable ---------------------------------------------------------
edge264mvc_test$(EXE): src/edge264mvc_test.c edge264mvc.h src/edge264mvc_internal.h $(LIBNAME)
	$(Q)$(CCLD) src/edge264mvc_test.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# ---- Object files ------------------------------------------------------------
edge264mvc.o: edge264mvc.h src/*
	$(Q)$(CC) src/edge264mvc.c -c $(CPPFLAGS) $(CFLAGS) $(OBJFLAGS) $(RUNTIME_TESTS) -o $@

edge264mvc_headers_v2.o: edge264mvc.h src/*
	$(Q)$(CC) src/edge264mvc_headers.c -c $(CPPFLAGS) $(CFLAGS) $(OBJFLAGS) -march=x86-64-v2 "-DADD_VARIANT(f)=f##_v2" -o $@

edge264mvc_headers_v3.o: edge264mvc.h src/*
	$(Q)$(CC) src/edge264mvc_headers.c -c $(CPPFLAGS) $(CFLAGS) $(OBJFLAGS) -march=x86-64-v3 "-DADD_VARIANT(f)=f##_v3" -o $@

edge264mvc_headers_v4.o: edge264mvc.h src/*
	$(Q)$(CC) src/edge264mvc_headers.c -c $(CPPFLAGS) $(CFLAGS) $(OBJFLAGS) -march=x86-64-v4 "-DADD_VARIANT(f)=f##_v4" -o $@

edge264mvc_headers_log.o: edge264mvc.h src/*
	$(Q)$(CC) src/edge264mvc_headers.c -c $(CPPFLAGS) $(CFLAGS) $(OBJFLAGS) -DLOGS "-DADD_VARIANT(f)=f##_log" -o $@


# ==============================================================================
# Install / Uninstall
# Installs the library, the public header, and a pkg-config .pc file.
# Use DESTDIR for staged installs (e.g. package manager sandboxes).
# ==============================================================================
.PHONY: install
install: $(LIBNAME)
	$(Q)install -d $(DESTDIR)$(libdir) $(DESTDIR)$(includedir) $(DESTDIR)$(libdir)/pkgconfig
	$(Q)install -m 644 $(LIBNAME) $(DESTDIR)$(libdir)/
	$(Q)install -m 644 edge264mvc.h  $(DESTDIR)$(includedir)/
ifeq ($(OS),linux)
  ifneq ($(STATIC),yes)
	$(Q)ln -sf $(LIBNAME) $(DESTDIR)$(libdir)/libedge264mvc.so
	$(Q)ldconfig $(DESTDIR)$(libdir) 2>/dev/null || true
  endif
endif
ifeq ($(OS),macos)
  ifneq ($(STATIC),yes)
	$(Q)ln -sf $(LIBNAME) $(DESTDIR)$(libdir)/libedge264mvc.dylib
  endif
endif
ifeq ($(OS),windows)
  ifneq ($(STATIC),yes)
	$(Q)install -m 644 libedge264mvc.dll.a $(DESTDIR)$(libdir)/
  endif
endif
	$(Q)( \
	  echo 'prefix=$(PREFIX)'; \
	  echo 'exec_prefix=$${prefix}'; \
	  echo 'libdir=$(libdir)'; \
	  echo 'includedir=$(includedir)'; \
	  echo ''; \
	  echo 'Name: edge264mvc'; \
	  echo 'Description: H.264 and H.264 MVC (3D) video decoder'; \
	  echo 'Version: $(VERSION)'; \
	  echo 'Libs: -L$${libdir} -ledge264mvc'; \
	  $(if $(_THREAD_FLAG),echo 'Libs.private: $(_THREAD_FLAG)';) \
	  echo 'Cflags: -I$${includedir}'; \
	) > $(DESTDIR)$(libdir)/pkgconfig/edge264mvc.pc

.PHONY: uninstall
uninstall:
	$(Q)rm -f $(DESTDIR)$(libdir)/$(LIBNAME) \
	          $(DESTDIR)$(libdir)/libedge264mvc.so \
	          $(DESTDIR)$(libdir)/libedge264mvc.dylib \
	          $(DESTDIR)$(libdir)/pkgconfig/edge264mvc.pc \
	          $(DESTDIR)$(includedir)/edge264mvc.h


# ==============================================================================
# Clean
# ==============================================================================
.PHONY: clean clear
clean clear:
	$(Q)rm -f edge264mvc_test edge264mvc_test.exe edge264mvc_test.js edge264mvc_test.wasm edge264mvc_check edge264mvc_check.exe edge264mvc_check.js edge264mvc_check.wasm conformance_check conformance_check.exe liveness_check liveness_check.exe asan_check asan_check.exe frame_info frame_info.exe api_check api_check.exe multi_decoder_check multi_decoder_check.exe slice_overrun_check slice_overrun_check.exe open_failure_check open_failure_check.exe alloc_failure_check alloc_failure_check.exe partial_receive_check partial_receive_check.exe conformance_check_stall liveness_check_stall edge264mvc_test_stall static_plugin.so static_plugin.dll fuzz_decode edge264*.o libedge264mvc.a edge264mvc.$(MAJOR).dll libedge264mvc.dll.a edge264mvc.js edge264mvc.wasm libedge264mvc.$(MAJOR).dylib libedge264mvc-universal.$(MAJOR).dylib libedge264mvc.so libedge264mvc.so.$(MAJOR)


# ==============================================================================
# Automated tests
# ==============================================================================
.PHONY: check
check: edge264mvc_check$(EXE)
ifeq ($(OS),wasm)
	# Older Node hides relaxed SIMD behind --experimental-wasm-relaxed-simd;
	# newer Node enables it by default and removed the flag (passing it aborts
	# with "bad option"). Probe once and only pass the flag when accepted.
	$(Q)NODE="$(shell which node)"; \
	  if "$$NODE" --experimental-wasm-relaxed-simd -e '' >/dev/null 2>&1; then \
	    "$$NODE" --experimental-wasm-relaxed-simd edge264mvc_check$(EXE); \
	  else \
	    "$$NODE" edge264mvc_check$(EXE); \
	  fi
else
	$(Q)./edge264mvc_check$(EXE)
	$(Q)$(MAKE) --no-print-directory check-conformance
	$(Q)$(MAKE) --no-print-directory check-stream-input
	$(Q)$(MAKE) --no-print-directory check-edge264mvc-test-liveness
	$(Q)$(MAKE) --no-print-directory check-stress STRESS_RUNS=20
	$(Q)$(MAKE) --no-print-directory check-edge264mvc-test-yuv
	$(Q)$(MAKE) --no-print-directory check-robustness
	$(Q)$(MAKE) --no-print-directory check-api
	$(Q)$(MAKE) --no-print-directory check-multi-decoder
	$(Q)$(MAKE) --no-print-directory check-slice-overrun
	$(Q)$(MAKE) --no-print-directory check-open-failure
	$(Q)$(MAKE) --no-print-directory check-alloc-failure
	$(Q)$(MAKE) --no-print-directory check-partial-receive
	$(Q)$(MAKE) --no-print-directory check-harness-stall
	$(Q)$(MAKE) --no-print-directory check-test-results
	$(Q)$(MAKE) --no-print-directory check-harness-manifest
endif

edge264mvc_check$(EXE): src/edge264mvc_check.c edge264mvc.h src/edge264mvc_internal.h $(LIBNAME)
	$(Q)$(CCLD) src/edge264mvc_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# Committed decode-regression over the bundled JVT conformance fixtures
# (tests/conformance/). Run by `check` on every non-wasm target; also
# runnable standalone. Needs no reference YUVs - the manifest carries
# the expected per-view hashes, so a fresh clone runs it fully offline. The
# second pass holds every fixture to the same values when frames are received
# only once the decoder is full (CONFORMANCE_PACED).
.PHONY: check-conformance
check-conformance: conformance_check$(EXE)
	$(Q)./conformance_check$(EXE) run tests/conformance/manifest.txt tests/conformance
	$(Q)CONFORMANCE_PACED=1 ./conformance_check$(EXE) run tests/conformance/manifest.txt tests/conformance
	$(Q)$(MAKE) --no-print-directory check-conformance-mt
	$(Q)$(MAKE) --no-print-directory check-conformance-trace
	$(Q)$(MAKE) --no-print-directory check-liveness

# The same fixtures with the header trace on (a log callback that discards the
# lines): formatting the trace must neither abort nor change the output.
.PHONY: check-conformance-trace
check-conformance-trace: conformance_check$(EXE)
	$(Q)EDGE264MVC_TRACE=1 ./conformance_check$(EXE) run tests/conformance/manifest.txt tests/conformance

# Multithreaded bit-exactness: decode the same fixtures with background worker
# threads and assert the per-view hashes still equal the (single-thread / ITU
# anchored) manifest. Proves the multithreaded path is bit-identical to
# single-thread output, and a stall fails here through the bound the harness
# puts on rounds of AGAIN without a frame (check-harness-stall). Skipped on wasm
# (single-threaded runtime). The EDGE264MVC_THREADS=-1 pass also exercises the
# auto-detect (logical-core) spawn+teardown path: it must persist its resolved
# count so edge264mvc_close joins every worker before freeing (a -1 left in
# dec->n_threads skips the join -> teardown access violation, esp. on Windows).
.PHONY: check-conformance-mt
check-conformance-mt: conformance_check$(EXE)
ifneq ($(OS),wasm)
	$(Q)EDGE264MVC_THREADS=8 ./conformance_check$(EXE) run tests/conformance/manifest.txt tests/conformance
	$(Q)EDGE264MVC_THREADS=-1 ./conformance_check$(EXE) run tests/conformance/manifest.txt tests/conformance
endif

conformance_check$(EXE): tests/conformance_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/conformance_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# Committed liveness regression over the bundled damaged-stream fixtures
# (tests/liveness/). Decodes each with a progress guard and asserts it delivers
# the expected base-frame count without stalling - catches decode-deadlock bugs
# a hash comparison cannot express. Run by `check`; also runnable standalone.
.PHONY: check-liveness
check-liveness: liveness_check$(EXE)
	$(Q)./liveness_check$(EXE) run tests/liveness/manifest.txt tests/liveness
ifneq ($(OS),wasm)
	$(Q)EDGE264MVC_THREADS=8 ./liveness_check$(EXE) run tests/liveness/manifest.txt tests/liveness
	$(Q)EDGE264MVC_THREADS=-1 ./liveness_check$(EXE) run tests/liveness/manifest.txt tests/liveness
endif

liveness_check$(EXE): tests/liveness_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/liveness_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# Native edge264mvc_test stream-input regression. Compares regular-file mmap,
# stdin (-), and FIFO/non-regular input Y4M output on small MVC fixtures. FIFO
# input splits an Annex B start code across reads, and every subprocess has a timeout.
.PHONY: check-stream-input
check-stream-input: edge264mvc_test$(EXE)
ifneq ($(OS),wasm)
	$(Q)$(PY) tests/stream_input_check.py --exe ./edge264mvc_test$(EXE)
endif

# Exercise edge264mvc_test's mapped and streamed progress guards on a DPB full of
# unfinished pictures. A timeout turns a regressed ENOBUFS spin into a failure.
.PHONY: check-edge264mvc-test-liveness
check-edge264mvc-test-liveness: edge264mvc_test$(EXE)
ifneq ($(OS),wasm)
	$(Q)$(PY) tests/edge264mvc_test_liveness.py --exe ./edge264mvc_test$(EXE)
endif

# edge264mvc_test's comparison with a reference YUV must see every sample: a
# change at any corner of any plane of a frame, cropped or not, must FAIL.
.PHONY: check-edge264mvc-test-yuv
check-edge264mvc-test-yuv: edge264mvc_test$(EXE)
ifneq ($(OS),wasm)
	$(Q)$(PY) tests/edge264mvc_test_yuv_check.py --exe ./edge264mvc_test$(EXE)
endif

# Sanitizer regression over the crafted-SEI fixtures (tests/asan/). Build with a
# sanitizer to instrument it, e.g. `make SANITIZE=address check-asan` (or
# address,undefined). The harness decodes each fixture with a log callback
# (parse_sei only runs in the logging path) under a timeout, catching the
# crafted-bitstream memory-safety (M2) and unbounded-loop (M5) regressions a
# hash/liveness run cannot. Standalone (not part of `check`): the sanitizer
# build is heavier and opt-in. Run with SANITIZE empty it just decodes the
# fixtures uninstrumented (the OOB read is then a benign, undetected read).
# Leak detection stays on, as in the sanitizer job of CI: a decoder-owned NAL
# copy that no task frees only shows as a leak.
# a wall-clock bound for the harnesses that guard hangs, where the system has
# timeout (GNU coreutils; macOS has none by default, so it runs unbounded there)
TIMEOUT := $(if $(shell command -v timeout 2>/dev/null),timeout 90)

# The promises of the public API itself (version, defaults, INVALID results,
# END after send_end, a flush after the end, pts / user_data passthrough, a
# caller holding its latest frame through an end of sequence and a change of
# the frame size), on one MVC and one 2D stream, single-threaded and with four
# worker threads.
.PHONY: check-api
check-api: api_check$(EXE)
	$(Q)$(TIMEOUT) ./api_check$(EXE) tests/conformance/mvc/MVCDS-5.264 tests/conformance/2d-synthetic/crop_top_left.264 tests/crop-margin.264 tests/conformance/2d/CABA3_Sony_C.264 --seek tests/conformance/mvc/MVCDS-5.264 tests/conformance/mvc-jmvc/interview_colocated.264

api_check$(EXE): tests/api_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/api_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# Several decoders in several threads at once (the API allows it): each run
# must give the frame count of a run alone. Run under ThreadSanitizer in CI.
.PHONY: check-multi-decoder
check-multi-decoder: multi_decoder_check$(EXE)
	$(Q)$(TIMEOUT) ./multi_decoder_check$(EXE) tests/conformance/mvc/MVCDS-5.264 9 tests/conformance/2d/CABA3_Sony_C.264 300

multi_decoder_check$(EXE): tests/multi_decoder_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/multi_decoder_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# Slices that run past the start of the next slice while a worker thread
# decodes them before the parser has seen it, fed with pauses that let them get
# there (see tests/slice_overrun_check.c): every run must give the
# single-threaded output. Run under ThreadSanitizer in CI.
.PHONY: check-slice-overrun
check-slice-overrun: slice_overrun_check$(EXE)
	$(Q)$(TIMEOUT) ./slice_overrun_check$(EXE) tests/conformance/2d-synthetic/overrun_reference.264 8 300 20
	$(Q)$(TIMEOUT) ./slice_overrun_check$(EXE) tests/conformance/2d-synthetic/overrun_reference_422.264 8 300 20

slice_overrun_check$(EXE): tests/slice_overrun_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/slice_overrun_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# edge264mvc_open when creating one of its worker threads fails (see
# tests/open_failure_check.c). It interposes pthread_create, hence Linux only.
# Run under the sanitizers in CI.
.PHONY: check-open-failure
check-open-failure:
ifeq ($(OS),linux)
	$(Q)$(MAKE) --no-print-directory open_failure_check
	$(Q)$(TIMEOUT) ./open_failure_check
endif

open_failure_check: tests/open_failure_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/open_failure_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -ldl -o $@

# Decoding when the memory for a new picture cannot be allocated (see
# tests/alloc_failure_check.c), single-threaded and with four worker threads.
# It interposes aligned_alloc, hence Linux only.
.PHONY: check-alloc-failure
check-alloc-failure:
ifeq ($(OS),linux)
	$(Q)$(MAKE) --no-print-directory alloc_failure_check
	$(Q)$(TIMEOUT) ./alloc_failure_check tests/conformance/mvc/MVCDS-5.264
	$(Q)EDGE264MVC_THREADS=4 $(TIMEOUT) ./alloc_failure_check tests/conformance/mvc/MVCDS-5.264
endif

alloc_failure_check: tests/alloc_failure_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/alloc_failure_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -ldl -o $@

# A caller receiving one frame per round and holding up to a few frames, on a
# damaged MVC stream that fills the DPB (see tests/partial_receive_check.c),
# single-threaded and with four worker threads.
# conformance_check against a decoder that never makes progress
# (tests/stall_stub.c) must fail rather than hang: on AGAIN without a frame in
# both consumer models, and on a forked paced run that never answers, at its
# deadline. liveness_check must report a decode that aborts as "crashed", and
# edge264mvc_test must count a stalled stream as a FAIL. Linux only, as the stub
# blocks with pause().
.PHONY: check-harness-stall
check-harness-stall:
ifeq ($(OS),linux)
	$(Q)$(CC) -I. tests/conformance_check.c tests/stall_stub.c $(CPPFLAGS) $(CFLAGS) -o conformance_check_stall
	$(Q)out=$$($(TIMEOUT) ./conformance_check_stall run tests/conformance/manifest.txt tests/conformance); \
	  test $$? -eq 1 && echo "$$out" | grep -q 'stall: send_nal' || { echo "harness stall check FAILED (AGAIN)"; exit 1; }
	$(Q)grep ' 1$$' tests/conformance/manifest.txt | head -n 1 > conformance_check_stall.txt; \
	  out=$$(STALL_STUB_BLOCK=1 CONFORMANCE_TIMEOUT=2 $(TIMEOUT) ./conformance_check_stall run conformance_check_stall.txt tests/conformance); \
	  status=$$?; rm -f conformance_check_stall.txt; \
	  test $$status -eq 1 && echo "$$out" | grep -q 'no result within' || { echo "harness stall check FAILED (deadlock)"; exit 1; }
	$(Q)$(CC) -I. tests/liveness_check.c tests/stall_stub.c $(CPPFLAGS) $(CFLAGS) -o liveness_check_stall
	$(Q)grep -w '^self_reference' tests/liveness/manifest.txt > liveness_check_stall.txt; \
	  out=$$(STALL_STUB_ABORT=1 $(TIMEOUT) ./liveness_check_stall run liveness_check_stall.txt tests/liveness); \
	  status=$$?; rm -f liveness_check_stall.txt; \
	  test $$status -eq 1 && echo "$$out" | grep -q 'crashed' || { echo "harness stall check FAILED (liveness_check did not report a crash)"; exit 1; }
	$(Q)$(CC) -I. src/edge264mvc_test.c tests/stall_stub.c $(CPPFLAGS) $(CFLAGS) -o edge264mvc_test_stall
	$(Q)$(TIMEOUT) ./edge264mvc_test_stall -s -y tests/conformance/2d/CABA3_Sony_C.264 > /dev/null 2>&1; \
	  test $$? -eq 1 || { echo "harness stall check FAILED (edge264mvc_test passed a stalled file)"; exit 1; }
	$(Q)$(TIMEOUT) ./edge264mvc_test_stall -s -y - < tests/conformance/2d/CABA3_Sony_C.264 > /dev/null 2>&1; \
	  test $$? -eq 1 || { echo "harness stall check FAILED (edge264mvc_test passed a stalled stream input)"; exit 1; }
	$(Q)echo "harness stall check PASS"
endif

# edge264mvc_test must count every input it does not decode completely as a
# FAIL and exit 1: a decoder failing with EDGE264MVC_NOMEM (tests/stall_stub.c),
# a file it cannot open, a file named without the .264 suffix and a directory
# it cannot list (unless run as root, who may list it); and a trace it cannot
# write (here trace.yaml is a directory) must end it with exit 1, not a crash.
# -b must report a peak memory of at least 1 MB for a decode. Linux only, like
# the stub.
.PHONY: check-test-results
check-test-results: edge264mvc_test$(EXE)
ifeq ($(OS),linux)
	$(Q)rm -rf harness_trace && mkdir -p harness_trace/trace.yaml && \
	  (cd harness_trace && $(TIMEOUT) ../edge264mvc_test$(EXE) -v -y ../tests/conformance/2d/CABA3_Sony_C.264 > /dev/null 2>&1); \
	  status=$$?; rm -rf harness_trace; \
	  test $$status -eq 1 || { echo "edge264mvc_test results check FAILED (an unwritable trace gave exit status $$status)"; exit 1; }
	$(Q)mem=$$(./edge264mvc_test$(EXE) -b -y tests/conformance/2d/CABA3_Sony_C.264 2>&1 | sed -n 's/^memory: \([0-9.]*\)MB$$/\1/p'); \
	  awk "BEGIN { exit !($${mem:-0} >= 1) }" || { echo "edge264mvc_test results check FAILED (-b reported $${mem:-no} MB of peak memory)"; exit 1; }
	$(Q)if [ "$$(id -u)" != 0 ]; then rm -rf harness_dir && mkdir harness_dir && chmod 111 harness_dir && \
	  { $(TIMEOUT) ./edge264mvc_test$(EXE) harness_dir > /dev/null 2>&1; status=$$?; chmod 755 harness_dir; rm -rf harness_dir; \
	  test $$status -eq 1 || { echo "edge264mvc_test results check FAILED (an unlistable directory gave exit status $$status)"; exit 1; }; }; fi
	$(Q)$(CC) -I. src/edge264mvc_test.c tests/stall_stub.c $(CPPFLAGS) $(CFLAGS) -o edge264mvc_test_stall
	$(Q)STALL_STUB_NOMEM=1 $(TIMEOUT) ./edge264mvc_test_stall -s -y tests/conformance/2d/CABA3_Sony_C.264 > /dev/null 2>&1; \
	  test $$? -eq 1 || { echo "edge264mvc_test results check FAILED (EDGE264MVC_NOMEM did not FAIL)"; exit 1; }
	$(Q)$(TIMEOUT) ./edge264mvc_test_stall tests/conformance/2d/does-not-exist.264 > /dev/null 2>&1; \
	  test $$? -eq 1 || { echo "edge264mvc_test results check FAILED (a missing file did not FAIL)"; exit 1; }
	$(Q)$(TIMEOUT) ./edge264mvc_test_stall tests/conformance/manifest.txt > /dev/null 2>&1; \
	  test $$? -eq 1 || { echo "edge264mvc_test results check FAILED (a file without the .264 suffix did not FAIL)"; exit 1; }
	$(Q)$(TIMEOUT) ./edge264mvc_test$(EXE) -e -y tests/conformance/2d-synthetic/aux_picture.264 2>&1 | grep -q "1 .*PASS.*, 0 .*UNSUPPORTED" || \
	  { echo "edge264mvc_test results check FAILED (the NAL units of an auxiliary picture were not ignored)"; exit 1; }
	$(Q){ printf '\000\000\001\030\200'; cat tests/conformance/2d/CABA3_Sony_C.264; } | $(TIMEOUT) ./edge264mvc_test$(EXE) -y - 2>&1 | grep -q "1 .*PASS.*, 0 .*UNSUPPORTED" || \
	  { echo "edge264mvc_test results check FAILED (an unspecified NAL unit was not skipped)"; exit 1; }
	$(Q){ printf '\000\000\001\030\200'; cat tests/conformance/2d/CABA3_Sony_C.264; } | $(TIMEOUT) ./edge264mvc_test$(EXE) -e -y - 2>&1 | grep -q "0 .*PASS.*, 1 .*UNSUPPORTED" || \
	  { echo "edge264mvc_test results check FAILED (-e did not stop at an unspecified NAL unit)"; exit 1; }
	$(Q)echo "edge264mvc_test results check PASS"
endif

# A manifest line conformance_check or liveness_check cannot read (a column
# missing, a count that is not a number) must fail the run, not drop its fixture;
# and conformance_check emit must report a base-view mismatch of an MVC stream
# whatever the dependent view's verdict.
.PHONY: check-harness-manifest
check-harness-manifest: conformance_check$(EXE) liveness_check$(EXE)
ifneq ($(OS),wasm)
	$(Q)printf '2d-synthetic/crop_top_left 3 0\n' > harness_manifest.txt; \
	  ./conformance_check$(EXE) run harness_manifest.txt tests/conformance > /dev/null; status=$$?; \
	  printf 'self_reference three\n' > harness_manifest.txt; \
	  ./liveness_check$(EXE) run harness_manifest.txt tests/liveness > /dev/null; status2=$$?; \
	  rm -f harness_manifest.txt; \
	  test $$status -eq 1 -a $$status2 -eq 1 || { echo "harness manifest check FAILED (a malformed line passed)"; exit 1; }
	$(Q)rm -rf harness_emit && mkdir -p harness_emit/mvc && cp tests/conformance/mvc/MVCDS-5.264 harness_emit/mvc/ && \
	  printf 'not the reference' > harness_emit/mvc/MVCDS-5.yuv && \
	  out=$$(./conformance_check$(EXE) emit harness_emit mvc/MVCDS-5); rm -rf harness_emit; \
	  echo "$$out" | grep -q 'check=BASE-MISMATCH dep_check=NOREF' || { echo "harness manifest check FAILED (emit hid a base-view mismatch)"; exit 1; }
	$(Q)echo "harness manifest check PASS"
endif

.PHONY: check-partial-receive
check-partial-receive: partial_receive_check$(EXE)
	$(Q)$(TIMEOUT) ./partial_receive_check$(EXE) tests/liveness/mvc_requeue_dependent.264 0
	$(Q)$(TIMEOUT) ./partial_receive_check$(EXE) tests/liveness/mvc_requeue_dependent.264 4
	$(Q)EDGE264MVC_THREADS=4 $(TIMEOUT) ./partial_receive_check$(EXE) tests/liveness/mvc_requeue_dependent.264 0

partial_receive_check$(EXE): tests/partial_receive_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/partial_receive_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# The same crafted fixtures without a sanitizer, single-threaded and with four
# worker threads, as part of `check`: they also guard hangs, some of which need
# every worker blocked at once (hence exactly four threads, more than any
# fixture keeps busy).
.PHONY: check-robustness
check-robustness: asan_check$(EXE)
	$(Q)$(TIMEOUT) ./asan_check$(EXE) run tests/asan/manifest.txt tests/asan
	$(Q)EDGE264MVC_THREADS=4 $(TIMEOUT) ./asan_check$(EXE) run tests/asan/manifest.txt tests/asan

.PHONY: check-asan
check-asan: asan_check$(EXE)
	$(Q)ASAN_OPTIONS=detect_leaks=1:max_allocation_size_mb=2048 $(TIMEOUT) ./asan_check$(EXE) run tests/asan/manifest.txt tests/asan

asan_check$(EXE): tests/asan_check.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/asan_check.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# Decode the fixtures STRESS_RUNS times each with worker threads, each run in a
# process of its own, against the single-threaded output: a race shows only in
# some runs, which a single decode per fixture rarely hits. `check` runs it 20
# times per fixture; the 500 runs of the default take minutes. A release build
# under a sanitizer finds the most, as an assertion no longer stops a worker
# there before the damage: `make clean` and then
# `make SANITIZE=address CPPFLAGS=-DNDEBUG check-stress`. The runs alternate
# with frame_info, which prints the order and flags of the views as well, as
# two dependent views paired with the wrong base views give the same samples.
STRESS_RUNS ?= 500
.PHONY: check-stress
check-stress: edge264mvc_test$(EXE) frame_info$(EXE)
ifneq ($(OS),wasm)
	$(Q)$(PY) tests/stress_check.py --exe ./edge264mvc_test$(EXE) --frame-info ./frame_info$(EXE) --runs $(STRESS_RUNS) tests/liveness/*.264 tests/asan/*.264 tests/conformance/mvc/*.264 tests/conformance/mvc-synthetic/*.264 tests/conformance/mvc-jmvc/*.264
endif

frame_info$(EXE): tests/frame_info.c edge264mvc.h $(LIBNAME)
	$(Q)$(CCLD) -I. tests/frame_info.c $(CPPFLAGS) $(CFLAGS) $(EXEFLAGS) -o $@

# libFuzzer harness under AddressSanitizer and UndefinedBehaviorSanitizer (needs
# clang), built from the sources so every function is instrumented. Assertions
# are compiled out like in a release build, so the fuzzer looks for what a
# release build would do with damaged input.
.PHONY: fuzz
fuzz: fuzz_decode$(EXE)
fuzz_decode$(EXE): tests/fuzz_decode.c edge264mvc.h src/*
	$(Q)clang -DNDEBUG -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -O1 -g -std=gnu11 -flax-vector-conversions -Wno-override-init -pthread $(filter -march=%,$(CFLAGS)) -I. -Isrc src/edge264mvc.c tests/fuzz_decode.c -o $@

.PHONY: gentests
gentests: $(TESTS_264)
%.264: %.yaml tests/gen_avc.py
	$(Q)$(PY) tests/gen_avc.py $< $@


# ==============================================================================
# Source archive
# Produces edge264-mvc-$(VERSION).tar.gz from the files tracked at git HEAD.
# Requires git; aborts with a git error message if not in a repository.
# ==============================================================================
.PHONY: dist
dist:
	$(Q)git archive --format=tar.gz --prefix=edge264-mvc-$(VERSION)/ \
	    -o edge264-mvc-$(VERSION).tar.gz HEAD


# ==============================================================================
# Quick help
# ==============================================================================
.PHONY: help
help:
	@echo ""
	@echo "Usage: make [TARGET] [PARAMETERS]"
	@echo ""
	@echo "Main targets:"
	@echo "  all         Build the library (+ test executable if BUILDTEST=yes)"
	@echo "  install     Install library, header and pkg-config file"
	@echo "  uninstall   Remove installed files"
	@echo "  clean       Remove build artifacts"
	@echo "  check       Run automated tests (edge264mvc_check)"
	@echo "  gentests    Generate .264 test bitstreams from .yaml files"
	@echo "  dist        Create edge264-mvc-$(VERSION).tar.gz from git HEAD"
	@echo "  help        Show this help"
	@echo ""
	@echo "Current parameters:"
	@echo "  CC=$(CC)"
	@echo "  CCLD=$(CCLD)"
	@echo "  AR=$(AR)"
	@echo "  OS=$(OS)"
	@echo "  VARIANTS=$(VARIANTS)"
	@echo "  SANITIZE=$(SANITIZE)"
	@echo "  STATIC=$(STATIC)"
	@echo "  PREFIX=$(PREFIX)"
	@echo "  libdir=$(libdir)"
	@echo "  includedir=$(includedir)"
	@echo "  DESTDIR=$(DESTDIR)"
	@echo "  CPPFLAGS=$(CPPFLAGS)"
	@echo "  CFLAGS=$(CFLAGS)"
	@echo "  LDFLAGS=$(LDFLAGS)"
	@echo "  OBJFLAGS=$(OBJFLAGS)"
	@echo "  LIBFLAGS=$(LIBFLAGS)"
	@echo "  EXEFLAGS=$(EXEFLAGS)"
	@echo "  BUILDTEST=$(BUILDTEST)"
	@echo "  V=$(V)"
	@echo "  PY=$(PY)"
	@echo ""
	@echo "Examples:"
	@echo "  make"
	@echo "  make V=yes"
	@echo "  make VARIANTS=x86-64-v2,x86-64-v3,x86-64-v4,logs"
	@echo "  make SANITIZE=address,undefined"
	@echo "  make STATIC=yes"
	@echo "  make OS=wasm VARIANTS=logs BUILDTEST=no"
	@echo "  make install PREFIX=\$$HOME/.local"
	@echo "  make install libdir=/usr/lib64"
	@echo "  make CPPFLAGS=-DNDEBUG LDFLAGS='-Wl,-z,relro -Wl,-z,now'"
	@echo "  make OS=linux CC=clang CCLD=aarch64-linux-gnu-gcc \\"
	@echo "       CFLAGS='--target=aarch64-linux-gnu --sysroot=...'"
	@echo ""
