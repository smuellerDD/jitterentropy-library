cmake_minimum_required(VERSION 3.22)

# From jitterentropy.h, as the Makefile does, so the package files cannot
# drift from the library version. A version bump reconfigures.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy.h)
foreach(_jent_comp MAJVERSION MINVERSION PATCHLEVEL)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy.h" _jent_line
         REGEX "^#define[ \t]+JENT_${_jent_comp}[ \t]+[0-9]+")
    string(REGEX MATCH "[0-9]+" JENT_${_jent_comp} "${_jent_line}")
    if(NOT DEFINED JENT_${_jent_comp} OR JENT_${_jent_comp} STREQUAL "")
        message(FATAL_ERROR "Cannot parse JENT_${_jent_comp} from jitterentropy.h")
    endif()
endforeach()

project(jitterentropy
    LANGUAGES C
    VERSION ${JENT_MAJVERSION}.${JENT_MINVERSION}.${JENT_PATCHLEVEL}
)
set(CMAKE_C_STANDARD 11)
# -std=c11 as the Makefile passes, not CMake's default -std=gnu11: the sources
# define the feature test macros they need themselves (see
# arch/jitterentropy-arch-compat.h), and the two builds of one library should
# not see different system headers.
set(CMAKE_C_EXTENSIONS OFF)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(STACK_PROTECTOR "Compile Jitter with stack protector enabled" ON)
option(INTERNAL_TIMER "Compile Jitter with the internal thread based timer" ON)
option(EXTERNAL_CRYPTO "Compile Jitter and use an external libcrypto, valid options are [AWSLC, OPENSSL, LIBGCRYPT]")
option(AARCH64_NSTIME_REGISTER "Name of the register that should be used in jent_get_nstime function")
option(ENABLE_SANITIZERS "Compile with address and undefined behavior sanitizers (only for development)" OFF)
option(ENABLE_THREAD_SANITIZER "Compile with the thread sanitizer, which is what unit-concurrency is written for (only for development)" OFF)
option(ENABLE_COVERAGE "Instrument the build for code coverage and add the 'coverage' target (only for development)" OFF)
option(ENABLE_FUZZING "Instrument the build for libFuzzer and build the fuzzing harness under tests/fuzz (Clang only, for development)" OFF)
option(MOCK_TIMER "Allow the time source to be replaced by a caller-supplied callback (only for testing - a build with this on produces no entropy of its own)" OFF)
option(ENABLE_RECORDING "Also build and install libjitterentropy-record: the library with the raw noise recording API of jitterentropy-record.h added, for recording where no tool can run (the Android and iOS example apps); not for production" OFF)
option(ENABLE_KERNEL_LIB "Also build and install libjitterentropy-kernel and the jitter_rng tool: user space access to the jitter_rng Linux kernel module (Linux only)" OFF)
option(ENABLE_RNGD "Also build and install jitterentropy-rngd, the daemon feeding the Jitter RNG into the Linux /dev/random, and its systemd unit (Linux only)" OFF)
option(BUILD_TESTING "Build the test programs and register the CTest suite" ON)

# The tools are command line programs. On the Apple platforms other than macOS
# CMake builds every executable as an app bundle, which install(TARGETS) needs a
# BUNDLE DESTINATION for, and the configure stopped at the first tool - so they
# are off there unless asked for, as INSTALL_MAN below is.
if(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS|visionOS)$")
    set(_jent_enable_tools_default OFF)
else()
    set(_jent_enable_tools_default ON)
endif()
option(ENABLE_TOOLS "Build and install the recording and validation tools (jitterentropy-rng, getrawentropy, extractlsb, ...), which live under tests/" ${_jent_enable_tools_default})

# The man pages are generated and installed only for a target that has man to
# display them: not Windows (MinGW included - Cygwin and MSYS are not WIN32 and
# keep them), Android, the Apple platforms other than macOS, bare metal or
# Emscripten. Settable either way, e.g. for a Windows package that ships the
# pages for another system to read.
if(WIN32 OR ANDROID OR CMAKE_SYSTEM_NAME MATCHES
   "^(iOS|tvOS|watchOS|visionOS|Generic|Emscripten)$")
    set(_jent_install_man_default OFF)
else()
    set(_jent_install_man_default ON)
endif()
option(INSTALL_MAN "Generate and install the man pages and their aliases" ${_jent_install_man_default})

# CMake sets MSVC itself; do the same for the others.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(CLANG 1)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(GCC 1)
elseif(CMAKE_C_COMPILER_ID MATCHES "SunPro")
    set(SUNPRO 1)
endif()

# Built up with list(APPEND) only, so each starts out empty here: an unset
# variable reads the parent scope's value, or a -D cache entry, and a project
# that add_subdirectory()s this one - tests/ios/CMakeLists.txt does - would
# otherwise hand its own JITTER_LINK_LIBRARIES or JITTER_C_FLAGS to the
# library's link and compile, as JITTER_PC_REQUIRES_PRIVATE below explains.
set(JITTER_C_FLAGS "")
set(JITTER_LINKER_FLAGS "")
set(JITTER_LINK_LIBRARIES "")

if(INTERNAL_TIMER)
    list(APPEND JITTER_C_FLAGS -DJENT_CONF_ENABLE_INTERNAL_TIMER)
endif()

# Loud on purpose: such a build measures whatever the caller hands it rather
# than the machine.
if(MOCK_TIMER)
    list(APPEND JITTER_C_FLAGS -DJENT_CONF_ENABLE_MOCK_TIMER)
    message(WARNING
        "MOCK_TIMER is on: the time source can be replaced by the caller, so "
        "this build produces no entropy of its own. For testing only.")
endif()

if(EXTERNAL_CRYPTO)
    list(APPEND JITTER_C_FLAGS  -D${EXTERNAL_CRYPTO})
endif()

if(AARCH64_NSTIME_REGISTER)
    list(APPEND JITTER_C_FLAGS -DAARCH64_NSTIME_REGISTER="${AARCH64_NSTIME_REGISTER}")
endif()

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# The thread sanitizer, which is what tests/unit/unit-concurrency.c is written
# for: without it that program checks the outcome of running several instances
# at once, and with it also how the state they share is reached. The races the
# atomics in arch/jitterentropy-arch-atomic.h answer were found this way, so
# this build is what keeps them answered.
#
# A build of its own rather than an addition to ENABLE_SANITIZERS: the address
# and the thread sanitizer cannot be combined, and a run under one says nothing
# about the other.
#
# -fno-omit-frame-pointer so the reports name the call that raced rather than
# the frame it was inlined into, and -g so they name a line.
if(ENABLE_THREAD_SANITIZER)
    if(ENABLE_SANITIZERS)
        message(FATAL_ERROR
            "ENABLE_THREAD_SANITIZER and ENABLE_SANITIZERS are separate "
            "builds: the thread sanitizer cannot be combined with the address "
            "sanitizer. Configure two build directories instead.")
    endif()
    if(NOT (GCC OR CLANG))
        message(FATAL_ERROR
            "ENABLE_THREAD_SANITIZER requires GCC or Clang, have "
            "${CMAKE_C_COMPILER_ID}")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)

    # The suite needs one suppression to be readable - the counting thread of
    # the internal timer is an unsynchronized counter on purpose, and it is
    # reported by every test that has no usable clock to avoid it. Passed
    # through the environment rather than set on the tests here: CMake 3.22
    # scopes test names to the directory that registered them, and these are
    # registered in five subdirectories.
    #
    # halt_on_error so the first report fails the test that produced it rather
    # than being counted and passed over.
    message(STATUS
        "ENABLE_THREAD_SANITIZER: run the suite with\n"
        "    TSAN_OPTIONS=halt_on_error=1:suppressions="
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp \\\n"
        "      ctest --test-dir <build> --output-on-failure -LE unreliable")
endif()

# The whole build is instrumented, not only the harness: libFuzzer guides
# itself by the coverage of the code under test, and a library compiled without
# the counters leaves it fuzzing blind. Only the harness links -fsanitize=fuzzer
# (it supplies main), which is why this is the -no-link spelling.
#
# Pair it with -DENABLE_SANITIZERS=ON for the memory errors a fuzzer is run to
# find; the harness checks the API contract on its own either way.
if(ENABLE_FUZZING)
    if(NOT CLANG)
        message(FATAL_ERROR
            "ENABLE_FUZZING requires Clang, have ${CMAKE_C_COMPILER_ID}")
    endif()
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR
            "ENABLE_FUZZING needs BUILD_TESTING: the harness is a test program")
    endif()

    # Compiled and linked, not only compiled: the instrumentation is part of
    # the compiler while the runtime that supplies main() is a separate
    # library, and a toolchain can have the first without the second - Apple's
    # Command Line Tools clang is the common one, and it fails at the link of
    # the harness, several minutes into the build.
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer)
    check_c_source_compiles("
        #include <stddef.h>
        #include <stdint.h>
        int LLVMFuzzerTestOneInput(const uint8_t *d, size_t s);
        int LLVMFuzzerTestOneInput(const uint8_t *d, size_t s)
        { (void)d; (void)s; return 0; }
    " JENT_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT JENT_HAVE_LIBFUZZER)
        message(FATAL_ERROR
            "ENABLE_FUZZING: this Clang compiles but cannot link "
            "-fsanitize=fuzzer - it ships no libFuzzer runtime. Apple's "
            "Command Line Tools clang is one such; build with an LLVM "
            "toolchain instead (-DCMAKE_C_COMPILER=/path/to/llvm/bin/clang).")
    endif()

    # Compile *and* link, as ENABLE_SANITIZERS and ENABLE_COVERAGE above do.
    # The instrumentation emits calls to __sanitizer_cov_* and a reference to
    # __sancov_lowest_stack, and something has to define them: libFuzzer does,
    # but only the two harnesses link it. Every other program here - the
    # recording tools, tests/gcd, tests/health, the unit tests - is instrumented
    # just the same and would fail to link with a few hundred undefined
    # references. On the link line -fsanitize=fuzzer-no-link supplies exactly
    # those definitions and no main(), which is what the -no-link spelling is
    # for.
    #
    # This went unnoticed while the option was only ever used together with
    # ENABLE_SANITIZERS, whose runtime defines the same symbols and so covered
    # for it.
    add_compile_options(-fsanitize=fuzzer-no-link)
    add_link_options(-fsanitize=fuzzer-no-link)
endif()

# add_*_options() as with ENABLE_SANITIZERS, to reach the test programs too.
# -O0 because the optimizer folds lines until the counters no longer map onto
# the source; --coverage on the link line pulls in the .gcda runtime.
if(ENABLE_COVERAGE)
    if(NOT (GCC OR CLANG))
        message(FATAL_ERROR
            "ENABLE_COVERAGE requires GCC or Clang, have ${CMAKE_C_COMPILER_ID}")
    endif()
    # The report comes from running the suite, so there has to be one.
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR
            "ENABLE_COVERAGE needs BUILD_TESTING: the coverage target runs "
            "the test suite")
    endif()
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# The library itself belongs in Libs whether it is shared or static: plain
# "pkg-config --libs" does not print Libs.private, and a static-only install
# must still name the archive there. Libs.private is for what the archive
# depends on (-pthread, the stack protector flag, -lbcrypt), which only a
# static link has to be told about; those are appended further down.
set(JITTER_PC_LIBS "-l${PROJECT_NAME}")
set(JITTER_PC_LIBS_PRIVATE "")

# Requires.private: the pkg-config modules a static link has to resolve as
# well, set only where one was found. Initialized here rather than only in the
# branches that set it, because this project can be add_subdirectory()'d -
# tests/ios/CMakeLists.txt does exactly that - and an unset variable in
# configure_file() takes the parent scope's value, so a JITTER_PC_REQUIRES_PRIVATE
# belonging to something else would be written into our .pc file. There is no
# "Requires:" counterpart: nothing is needed to *compile* against
# jitterentropy.h, which includes no other package's headers.
set(JITTER_PC_REQUIRES_PRIVATE "")

# Appended to the Cflags line of the .pc file. Only the Windows static build
# has anything to put here; see the JENT_STATIC_LIB branch further down.
set(JITTER_PC_CFLAGS "")

if(WIN32)
    if(MSVC)
        # MSVC deprecates the portable spellings for its own _s variants.
        list(APPEND JITTER_C_FLAGS  /Od /Ob0 /W4 /D_CRT_SECURE_NO_WARNINGS)

        # The entropy core must not be optimized, and on MSVC the source alone
        # cannot refuse it: the __OPTIMIZE__ guard in src/jitterentropy-base.c
        # is a GCC/Clang macro, and cl /O2 compiles that file clean. Appending
        # /Od is not enough either. /Od overrides /O2 (with warning D9025 on
        # every compile), but an explicit /Ob outlives it: CMake's Release
        # and RelWithDebInfo flags carry /Ob2 and /Ob1, and with them
        # jent_delta(), jent_udiv64() and uint32rotl() were inlined into the
        # timed loop - the instruction stream the entropy assessment describes
        # was not the one shipped. So the configuration flags are rewritten
        # rather than out-ordered: /O1, /O2 and /Ox become /Od, /Ob1-/Ob3
        # become /Ob0. This is directory scope, so it reaches the library and
        # every test below that compiles the sources into itself; a parent
        # project that add_subdirectory()s this one keeps its own flags. The
        # /Od /Ob0 in JITTER_C_FLAGS above still come last on each command
        # line, which covers an /O2 added through add_compile_options().
        #
        # /GL is refused. With it cl emits no machine code at all - code
        # generation is deferred to the linker under /LTCG, so what the timed
        # loop becomes is decided in a step no flag checked here witnesses.
        # Nothing here sets either; CMAKE_INTERPROCEDURAL_OPTIMIZATION would,
        # on a consumer's behalf.
        foreach(_var CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
                     CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL)
            if("${${_var}}" MATCHES "[-/]GL( |$)")
                message(FATAL_ERROR
                    "${_var} carries /GL: whole-program optimization defers "
                    "code generation to the linker, where the unoptimized "
                    "build the entropy core requires cannot be enforced. "
                    "Remove it.")
            endif()
            string(REGEX REPLACE "[-/]O[12x]( |$)" "/Od\\1" ${_var} "${${_var}}")
            string(REGEX REPLACE "[-/]Ob[123]( |$)" "/Ob0\\1" ${_var} "${${_var}}")
        endforeach()
        foreach(_var CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
                     CMAKE_STATIC_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS)
            foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
                if("${${_var}${_cfg}}" MATCHES "[-/]LTCG")
                    message(FATAL_ERROR
                        "${_var}${_cfg} carries /LTCG: link-time code "
                        "generation is where /GL defers the optimization the "
                        "entropy core must not have. Remove it.")
                endif()
            endforeach()
        endforeach()
        foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
            if(CMAKE_INTERPROCEDURAL_OPTIMIZATION${_cfg})
                message(FATAL_ERROR
                    "CMAKE_INTERPROCEDURAL_OPTIMIZATION${_cfg} is on: on MSVC "
                    "that is /GL and /LTCG, which defer code generation to the "
                    "linker, where the unoptimized build the entropy core "
                    "requires cannot be enforced. Turn it off.")
            endif()
        endforeach()
    else()
        list(APPEND JITTER_C_FLAGS  -fwrapv --param ssp-buffer-size=4 -fvisibility=hidden -Wcast-align
        -Wmissing-field-initializers -Wshadow -Wswitch-enum -Wextra -Wall -pedantic -O0 -fwrapv -Wconversion)
    endif()
else()
    list(APPEND JITTER_C_FLAGS  -fvisibility=hidden -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum -Wextra -Wall -pedantic -O0 -Wconversion)

    # The library returns a negated errno, which assumes positive constants.
    # Haiku's are its B_* codes, large negatives, so -ENOENT comes out positive
    # and every "if (ret < 0)" silently reads failure as success. This is
    # Haiku's own switch for that, and must precede every system header.
    if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        list(APPEND JITTER_C_FLAGS -DB_USE_POSITIVE_POSIX_ERRORS)
    endif()

    # Solaris' ld rejects -z relro but takes -z now, so it gets the
    # immediate-binding half only. Keyed on the system, not the compiler: GCC
    # there drives that same ld, so testing SUNPRO missed every GCC build.
    # ld64 understands neither form; Cygwin's targets PE/COFF and rejects -z.
    if(NOT APPLE AND NOT CYGWIN)
        if(SUNPRO OR CMAKE_SYSTEM_NAME STREQUAL "SunOS")
            list(APPEND JITTER_LINKER_FLAGS -Wl,-z,now)
        else()
            list(APPEND JITTER_LINKER_FLAGS -Wl,-z,now,-z,relro)
        endif()
    endif()
    # -pthread is a dependency rather than a hardening flag, and a static
    # consumer needs it as well - on FreeBSD nothing else names it - so it is
    # linked as a library, which the exported target carries as LINK_ONLY.
    if((CMAKE_SYSTEM_NAME STREQUAL "FreeBSD") OR (CMAKE_SYSTEM_NAME STREQUAL "OpenBSD") OR (CMAKE_SYSTEM_NAME STREQUAL "NetBSD"))
        list(APPEND JITTER_LINK_LIBRARIES -pthread)
    endif()
    if(SUNPRO)
        list(APPEND JITTER_C_FLAGS  -m64 -Kpic -errtags=yes -erroff=E_KW_IS_AN_EXTENSION_OF_ANSI,E_ASM_UNUSED_PARAM)
    else()
        list(APPEND JITTER_C_FLAGS  -fwrapv --param ssp-buffer-size=4 -fPIE -fPIC)
    endif()
endif()

set(JITTER_SSP_LINK_FLAGS "")
if(STACK_PROTECTOR)
    set(JITTER_SSP_FLAG "")
    if(GCC)
        if(CMAKE_C_COMPILER_VERSION VERSION_GREATER "4.9.0")
            set(JITTER_SSP_FLAG -fstack-protector-strong)
        else()
            set(JITTER_SSP_FLAG -fstack-protector-all)
        endif()
    # "NOT MSVC" excludes clang-cl, which is Clang - CMAKE_C_COMPILER_ID says so
    # and CLANG is set for it - behind a cl-compatible driver that does not take
    # GCC-style spellings. It answers -fstack-protector-strong with "unknown
    # argument ignored in clang-cl" and carries on, which the probe below reads
    # as success: the flag would be added to every compile and link command, be
    # dropped by each of them, and leave a build that reports hardening it does
    # not have. cl itself never reached here - neither GCC nor CLANG is set for
    # it - so this only ever concerned the one compiler that is both.
    #
    # Nothing is substituted for it. The equivalent is /GS, which clang-cl
    # enables by default exactly as cl does, so the MSVC builds already have the
    # protection this branch would otherwise ask for.
    elseif(CLANG AND NOT MSVC)
        set(JITTER_SSP_FLAG -fstack-protector-strong)
    endif()

    if(JITTER_SSP_FLAG)
        # The flag needs __stack_chk_fail/__stack_chk_guard, which most C
        # libraries define, Solaris' takes from GCC's libssp, and Haiku has
        # not at all. Hence the flag on the *link* command too: that is how the
        # driver is told to bring the runtime in. As a compile-only flag it
        # failed on Solaris, nothing pulling libssp in.
        #
        # A probe, not a CMAKE_SYSTEM_NAME check: illumos has both symbols yet
        # also reports SunOS. It needs a frame the flag instruments - -strong
        # guards only frames with a local array.
        include(CheckCSourceCompiles)
        set(JITTER_SSP_PROBE "
            int main(int argc, char **argv) {
                char buf[64];
                (void)argv;
                buf[0] = (char)argc;
                return buf[0];
            }")
        set(CMAKE_REQUIRED_FLAGS "${JITTER_SSP_FLAG}")
        check_c_source_compiles("${JITTER_SSP_PROBE}" JITTER_SSP_USABLE)
        unset(CMAKE_REQUIRED_FLAGS)

        # Hardening, not functionality, and on by default, so a toolchain
        # without the runtime (Haiku) must still build. Warned about rather
        # than dropped quietly.
        if(JITTER_SSP_USABLE)
            list(APPEND JITTER_C_FLAGS ${JITTER_SSP_FLAG})

            # Every target compiled with the flag needs it on its own link
            # command, and a shared library propagates nothing to the test
            # programs. add_link_options() reaches every target created after
            # it, tests/ included - but only in this build: it is a directory
            # property, not a target one, and nothing of it is exported.
            add_link_options(${JITTER_SSP_FLAG})

            # Kept for the static consumers, the exported CMake target and the
            # .pc file, both handled once the library target exists below.
            set(JITTER_SSP_LINK_FLAGS ${JITTER_SSP_FLAG})
        else()
            message(WARNING
                "Building WITHOUT ${JITTER_SSP_FLAG}: this toolchain cannot "
                "resolve __stack_chk_fail/__stack_chk_guard even with the flag "
                "on the link line, so it would break every link. Configure "
                "with -DSTACK_PROTECTOR=off to silence this.")
        endif()
    endif()
endif()

# CONFIGURE_DEPENDS so an added source cannot leave a stale configure
# producing an archive with missing objects.
file(GLOB JITTER_SRC CONFIGURE_DEPENDS "src/*.c" "arch/*.c")
add_library(${PROJECT_NAME})
# The name find_package() gives it, so that a project pulling this one in with
# add_subdirectory() or FetchContent links the same target name either way.
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE ${JITTER_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES
    PUBLIC_HEADER "jitterentropy.h"
    # As the Makefile: .so.<maj>.<min>.<patch>, SONAME .so.<maj>.
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# As the Makefile's -install_name: the installed dylib names itself by its
# absolute path, so the installed tools and any consumer linked through
# pkg-config load it without an LC_RPATH of their own. CMake's default,
# @rpath/, left every installed tool aborting with "Library not loaded" - the
# install tree carries no rpath to resolve it. $<INSTALL_PREFIX> is the prefix
# of the install, not of the configure, so "cmake --install --prefix" is
# honoured. The build tree keeps @rpath and CMake's build rpath.
if(APPLE)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
        set(_jent_install_name_dir "${CMAKE_INSTALL_LIBDIR}")
    else()
        set(_jent_install_name_dir "$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}")
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_NAME_DIR "${_jent_install_name_dir}")
endif()
target_compile_options(${PROJECT_NAME} PRIVATE ${JITTER_C_FLAGS})
# A link option, not a link library: target_link_libraries() on a static
# library puts even a PRIVATE item into INTERFACE_LINK_LIBRARIES as
# $<LINK_ONLY:...>, so install(EXPORT) handed -z now/-z relro to the link of
# every consumer of the archive. A PRIVATE link option stays on this target's
# own link, which only a shared build has.
target_link_options(${PROJECT_NAME} PRIVATE ${JITTER_LINKER_FLAGS})
# The programs built here - jitterentropy-rng and the other installed tools
# among them - need the same hardening on their own link, and in a static
# build they no longer inherit it from the archive. As with the stack
# protector above, add_link_options() is a directory property that reaches
# every target created after it, tests/ included, and nothing of it is
# exported. It is limited to executables: the library has its own option
# just above, and anything else created here is not linked by a linker.
add_link_options("$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${JITTER_LINKER_FLAGS}>")
target_link_libraries(${PROJECT_NAME} PRIVATE ${JITTER_LINK_LIBRARIES})

# version.lds is the exported set of a shared build. Only where the linker
# takes a GNU-style version script - ld64 and the Windows toolchains want their
# own - so it is probed, as the stack protector above is.
set(JITTER_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/version.lds")
if(BUILD_SHARED_LIBS AND NOT WIN32 AND NOT APPLE)
    include(CheckLinkerFlag)
    # With a script of its own: version.lds names symbols the probe does not
    # define, which lld refuses (--no-undefined-version, its default since
    # LLVM 16) - the probe then failed wherever lld links, FreeBSD's default
    # toolchain included, and the library went without the version script.
    set(JITTER_VERSION_SCRIPT_PROBE
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/jent-version-probe.lds")
    # With a C comment as version.lds has, which the Solaris link editor
    # refuses where it takes the option as a mapfile: the probe then fails
    # wherever version.lds would. A result name of its own, so a tree that
    # cached the old probe's verdict probes again.
    file(WRITE "${JITTER_VERSION_SCRIPT_PROBE}"
         "/* probe */\n{ global: main; local: *; };\n")
    check_linker_flag(C "-Wl,--version-script=${JITTER_VERSION_SCRIPT_PROBE}"
                      JITTER_VERSION_SCRIPT_LINKS)
    if(JITTER_VERSION_SCRIPT_LINKS)
        target_link_options(${PROJECT_NAME} PRIVATE
            "-Wl,--version-script=${JITTER_VERSION_SCRIPT}")
        # So a change to the list relinks the library.
        set_target_properties(${PROJECT_NAME} PROPERTIES
            LINK_DEPENDS "${JITTER_VERSION_SCRIPT}")
    else()
        message(STATUS
            "This linker takes no version script; the shared library exports "
            "whatever visibility leaves exported")
    endif()
endif()

# The link test proves that a symbol dropped from the export set fails a
# consumer's link - which it can only do for the functions it calls. Checked
# here because tests/linking is configured against an install tree and has
# neither the header nor the version script at hand.
function(jent_check_linktest_calls linktest api)
    file(READ "${linktest}" source)

    set(uncalled "")
    foreach(fn IN LISTS api)
        if(NOT source MATCHES "${fn}[ \t\r\n]*\\(")
            list(APPEND uncalled "${fn}")
        endif()
    endforeach()

    if(uncalled)
        message(FATAL_ERROR
            "tests/linking/jitterentropy-linktest.c does not call: ${uncalled}")
    endif()

    list(LENGTH api n)
    message(STATUS "the link test calls all ${n} of them")
endfunction()

# Nothing at build time would notice the two drifting: a declaration missing
# from the script just goes missing from the library. A function for the scope;
# block() is CMake 3.25 and this project asks for 3.22. Also called for
# libjitterentropy-record, whose header marks its API with JENT_RECORD_API and
# which has no link test: @linktest empty skips that check.
function(jent_check_version_script header script marker linktest)
    get_filename_component(header_name "${header}" NAME)
    get_filename_component(script_name "${script}" NAME)

    # Every exported function is declared as a bare @marker line followed by
    # the declaration. The declaration need not be on the very next
    # line - a long return type may stand on its own - so the lines after the
    # marker are read until one names a function, and anything that ends the
    # declaration before that is a parse failure rather than a silent skip: a
    # declaration this loop dropped would be compared against nothing, the two
    # lists would still agree, and a function omitted from version.lds as well
    # would quietly stop being exported on ELF.
    set(header_api "")
    set(pending "")
    set(pending_line 0)
    set(lineno 0)
    file(STRINGS "${header}" lines)
    foreach(line IN LISTS lines)
        math(EXPR lineno "${lineno} + 1")
        if(pending)
            if(line MATCHES "(jent_[A-Za-z0-9_]+)[ \t]*\\(")
                list(APPEND header_api "${CMAKE_MATCH_1}")
                set(pending "")
            elseif(line MATCHES "^[ \t]*$" OR line MATCHES "^[ \t]*[#/*]" OR
                   line MATCHES "^${marker}" OR line MATCHES ";")
                message(FATAL_ERROR
                    "${header}:${pending_line}: ${marker} is not "
                    "followed by a function declaration this parser "
                    "understands, so it would be left out of the comparison "
                    "with ${script}. Stopped at line ${lineno}: '${line}'")
            endif()
        elseif(line MATCHES "^${marker}$")
            set(pending TRUE)
            set(pending_line "${lineno}")
        endif()
    endforeach()
    if(pending)
        message(FATAL_ERROR
            "${header}:${pending_line}: ${marker} at the end of the "
            "file with no function declaration after it.")
    endif()

    # Only between "global:" and "local:", so the commentary above them is
    # not read as part of the list.
    set(script_api "")
    set(in_global FALSE)
    file(STRINGS "${script}" lines)
    foreach(line IN LISTS lines)
        if(line MATCHES "global:")
            set(in_global TRUE)
        elseif(line MATCHES "local:")
            set(in_global FALSE)
        elseif(in_global AND line MATCHES "(jent_[A-Za-z0-9_]+)")
            list(APPEND script_api "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    list(SORT header_api)
    list(SORT script_api)
    if(header_api STREQUAL script_api)
        list(LENGTH header_api n)
        message(STATUS
            "${script_name} exports the ${n} functions of ${header_name}")
        if(linktest)
            jent_check_linktest_calls("${linktest}" "${header_api}")
        endif()
        return()
    endif()

    set(only_header ${header_api})
    set(only_script ${script_api})
    if(script_api)
        list(REMOVE_ITEM only_header ${script_api})
    endif()
    if(header_api)
        list(REMOVE_ITEM only_script ${header_api})
    endif()
    message(FATAL_ERROR
        "${script_name} and ${header_name} disagree. "
        "Only in the header: ${only_header}. "
        "Only in the script: ${only_script}.")
endfunction()

jent_check_version_script("${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy.h"
                          "${JITTER_VERSION_SCRIPT}" JENT_PRIVATE_STATIC
                          "${CMAKE_CURRENT_SOURCE_DIR}/tests/linking/jitterentropy-linktest.c")

# As bcrypt below: a static consumer links this archive's instrumented frames
# and must resolve __stack_chk_fail/__stack_chk_guard itself, which on Solaris
# means the flag on its own link command. add_link_options() above is not
# exported, so the flag goes onto the target as an INTERFACE link option, where
# install(EXPORT) carries it into jitterentropyTargets.cmake; the .pc file gets
# it in Libs.private. A shared library resolved the symbols at its own link.
if(JITTER_SSP_LINK_FLAGS AND NOT BUILD_SHARED_LIBS)
    target_link_options(${PROJECT_NAME} INTERFACE ${JITTER_SSP_LINK_FLAGS})
    set(JITTER_PC_LIBS_PRIVATE "${JITTER_PC_LIBS_PRIVATE} ${JITTER_SSP_LINK_FLAGS}")
endif()

if(WIN32)
    # The UUID backend calls BCryptGenRandom(). MSVC finds the import library
    # through the #pragma comment(lib, ...) there, MinGW has to be told.
    target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt)

    # The same for pkg-config consumers; a shared build resolves it in the DLL.
    if(NOT BUILD_SHARED_LIBS)
        set(JITTER_PC_LIBS_PRIVATE "${JITTER_PC_LIBS_PRIVATE} -lbcrypt")
    endif()

    if(MSVC)
        # The Windows -z hardening. Linker defaults today, stated so a
        # toolchain changing them cannot drop ASLR or DEP; as compile options
        # they reached nothing. /GUARD:CF is deliberately absent - its compile
        # flag instruments the entropy core, whose timing is the noise source.
        target_link_options(${PROJECT_NAME} PRIVATE /DYNAMICBASE /NXCOMPAT)
    endif()

    # JENT_PRIVATE_STATIC is dllexport only while the library is compiled;
    # consumers get dllimport, or nothing for the static archive.
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(${PROJECT_NAME} PRIVATE JENT_BUILDING_DLL)
    else()
        target_compile_definitions(${PROJECT_NAME} PUBLIC JENT_STATIC_LIB)

        # The same for pkg-config consumers. PUBLIC above covers the exported
        # CMake target - the definition is carried in its
        # INTERFACE_COMPILE_DEFINITIONS - but nothing propagates it into the
        # .pc file, and without it the public header declares the functions
        # __declspec(dllimport) while the archive defines them undecorated, so
        # a MinGW consumer building through pkg-config was left looking for
        # __imp_ symbols that a static library never has.
        set(JITTER_PC_CFLAGS " -DJENT_STATIC_LIB")
    endif()
endif()
# Only jitterentropy.h. The arch/ headers are internal - the public header
# includes none of them and exports nothing they declare.
install(TARGETS ${PROJECT_NAME}
    EXPORT jitterentropyTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
# The package subdirectory in the destination is not cosmetic. find_package()
# in config mode searches <prefix>/<lib|share>/cmake/<name>*/ under every entry
# of CMAKE_PREFIX_PATH; a bare lib/cmake/ is not one of the places it looks, so
# a package installed directly there was only findable by pointing
# CMAKE_PREFIX_PATH at lib/cmake itself. tests/linking is the consumer that
# would have to do that.
install(EXPORT jitterentropyTargets
  FILE jitterentropyTargets.cmake
  NAMESPACE jitterentropy::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

target_include_directories(${PROJECT_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/arch>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

# The external crypto library. Found for what it is, not for how this library
# is linked: a static jitterentropy used to go looking for a static libcrypto.a,
# which most distributions do not install, and carried on with a NOTFOUND
# library until generation failed. Either linkage links whatever the system
# has, and a static archive's dependents are told what to add below.
#
# LIBCRYPTO_INCLUDE_DIR and LIBCRYPTO_LIBRARY are what the programs absorbing
# the library sources (tests/gcd, tests/unit, the AMALGAMATED recording tool)
# add for themselves. Linked PRIVATE here: jitterentropy.h includes none of
# these headers, so a consumer needs neither them nor, for a shared library,
# the link.
set(JENT_CONFIG_DEPENDENCIES "")
if(EXTERNAL_CRYPTO STREQUAL "AWSLC" OR EXTERNAL_CRYPTO STREQUAL "OPENSSL")
    # AWS-LC installs itself as libcrypto with OpenSSL's headers, so the one
    # module finds both; OPENSSL_ROOT_DIR chooses between them when both are
    # installed. On Windows it finds the import library, not the DLL.
    find_package(OpenSSL COMPONENTS Crypto)
    if(NOT OPENSSL_FOUND OR NOT TARGET OpenSSL::Crypto)
        message(FATAL_ERROR
            "EXTERNAL_CRYPTO=${EXTERNAL_CRYPTO}: no libcrypto found. Install "
            "its development files or set OPENSSL_ROOT_DIR to the prefix "
            "holding include/openssl and the library.")
    endif()
    # openssl/base.h is unique to AWS-LC (and BoringSSL): without it, what was
    # found is OpenSSL, and AWSLC would compile against the wrong API.
    if(EXTERNAL_CRYPTO STREQUAL "AWSLC" AND
       NOT EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/base.h")
        message(FATAL_ERROR
            "EXTERNAL_CRYPTO=AWSLC: the libcrypto found at ${OPENSSL_INCLUDE_DIR} "
            "is not AWS-LC (no openssl/base.h). Set OPENSSL_ROOT_DIR to the "
            "AWS-LC installation.")
    endif()
    set(LIBCRYPTO_INCLUDE_DIR "${OPENSSL_INCLUDE_DIR}")
    set(LIBCRYPTO_LIBRARY OpenSSL::Crypto)
    message(STATUS "Found external crypto library ${OPENSSL_CRYPTO_LIBRARY}")

    # A static archive exports the dependency as $<LINK_ONLY:OpenSSL::Crypto>,
    # a target the consumer has to create again.
    #
    # The .pc file names the libcrypto module only once pkg-config confirms it
    # exists, exactly as the LIBGCRYPT branch below does. FindOpenSSL does not
    # go through pkg-config, so nothing about it says the installation found
    # here ships a libcrypto.pc - a hand-built prefix reached through
    # OPENSSL_ROOT_DIR, and some vcpkg and AWS-LC layouts, do not. An
    # unresolvable name in Requires.private is not a static-link-only problem:
    # pkg-config resolves that field for --cflags and --exists too, so every
    # consumer of our .pc file would fail with "Package libcrypto was not
    # found". Where there is no module, the library goes into Libs.private by
    # its absolute path instead, which needs nothing found again.
    if(NOT BUILD_SHARED_LIBS)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(JENT_PC_LIBCRYPTO QUIET libcrypto)
        endif()
        if(JENT_PC_LIBCRYPTO_FOUND)
            set(JITTER_PC_REQUIRES_PRIVATE "libcrypto")
        else()
            # Filtered on EXISTS: FindOpenSSL reports the library as a plain
            # path, but on the multi-configuration generators it can be a
            # "optimized;<lib>;debug;<lib>" list, and those two keywords are
            # not linker arguments. -lcrypto if that leaves nothing at all.
            set(_jent_pc_crypto "")
            foreach(_jent_lib IN LISTS OPENSSL_CRYPTO_LIBRARY)
                if(EXISTS "${_jent_lib}")
                    set(_jent_pc_crypto "${_jent_pc_crypto} ${_jent_lib}")
                endif()
            endforeach()
            if(NOT _jent_pc_crypto)
                set(_jent_pc_crypto " -lcrypto")
            endif()
            set(JITTER_PC_LIBS_PRIVATE
                "${JITTER_PC_LIBS_PRIVATE}${_jent_pc_crypto}")
        endif()
        set(JENT_CONFIG_DEPENDENCIES "find_dependency(OpenSSL COMPONENTS Crypto)")
    endif()
elseif(EXTERNAL_CRYPTO STREQUAL "LIBGCRYPT")
    # libgpg-error by name as well: gcrypt.h includes gpg-error.h, and a static
    # libgcrypt.a leaves its symbols unresolved. pkg-config, where present, only
    # adds search locations - libgcrypt shipped no .pc file before 1.9, so the
    # enterprise distributions still in support have none.
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(JENT_PC_GCRYPT QUIET libgcrypt)
        pkg_check_modules(JENT_PC_GPG_ERROR QUIET gpg-error)
    endif()
    find_path(JENT_GCRYPT_INCLUDE_DIR NAMES gcrypt.h
              HINTS ${JENT_PC_GCRYPT_INCLUDE_DIRS})
    find_path(JENT_GPG_ERROR_INCLUDE_DIR NAMES gpg-error.h
              HINTS ${JENT_PC_GPG_ERROR_INCLUDE_DIRS} ${JENT_PC_GCRYPT_INCLUDE_DIRS})
    find_library(JENT_GCRYPT_LIBRARY NAMES gcrypt libgcrypt
                 HINTS ${JENT_PC_GCRYPT_LIBRARY_DIRS})
    find_library(JENT_GPG_ERROR_LIBRARY NAMES gpg-error libgpg-error
                 HINTS ${JENT_PC_GPG_ERROR_LIBRARY_DIRS} ${JENT_PC_GCRYPT_LIBRARY_DIRS})
    foreach(_jent_var JENT_GCRYPT_INCLUDE_DIR JENT_GPG_ERROR_INCLUDE_DIR
                      JENT_GCRYPT_LIBRARY JENT_GPG_ERROR_LIBRARY)
        if(NOT ${_jent_var})
            message(FATAL_ERROR
                "EXTERNAL_CRYPTO=LIBGCRYPT: ${_jent_var} not found. Install the "
                "libgcrypt and libgpg-error development files, or add their "
                "prefix to CMAKE_PREFIX_PATH.")
        endif()
    endforeach()
    set(LIBCRYPTO_INCLUDE_DIR
        "${JENT_GCRYPT_INCLUDE_DIR}" "${JENT_GPG_ERROR_INCLUDE_DIR}")
    list(REMOVE_DUPLICATES LIBCRYPTO_INCLUDE_DIR)
    set(LIBCRYPTO_LIBRARY "${JENT_GCRYPT_LIBRARY}" "${JENT_GPG_ERROR_LIBRARY}")
    message(STATUS "Found external crypto library ${JENT_GCRYPT_LIBRARY}")

    # Exported as absolute paths, which need nothing found again. The .pc file
    # names the module where there is one - its own Requires.private brings in
    # gpg-error - and the libraries where there is not.
    if(NOT BUILD_SHARED_LIBS)
        if(JENT_PC_GCRYPT_FOUND)
            set(JITTER_PC_REQUIRES_PRIVATE "libgcrypt")
        else()
            set(JITTER_PC_LIBS_PRIVATE "${JITTER_PC_LIBS_PRIVATE} -lgcrypt -lgpg-error")
        endif()
    endif()
elseif(NOT EXTERNAL_CRYPTO)
    message(STATUS "Using internal functions for everything")
else()
    message(FATAL_ERROR "Unknown EXTERNAL_CRYPTO option ${EXTERNAL_CRYPTO}")
endif()

if(EXTERNAL_CRYPTO)
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBCRYPTO_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBCRYPTO_LIBRARY})
endif()

# "NOT ANDROID", not "NOT ${ANDROID}": undefined, that leaves "AND NOT" with no
# operand and silently drops the dependency on every build.
#
# On Windows the timer uses native Win32 threads, so there is nothing to name -
# MSVC has no pthread.lib at all, and MinGW would pull in the winpthreads the
# Win32 back-end exists to avoid. Cygwin is POSIX and keeps pthreads.
if(INTERNAL_TIMER AND NOT ANDROID AND NOT WIN32)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_link_libraries(${PROJECT_NAME} PUBLIC pthread)
    endif()
endif()

# The same for the targets that cannot inherit it: the AMALGAMATED and unit
# test programs absorb the library sources, so they resolve the timer's
# pthread_create() - and clock_gettime(), below - themselves. -pthread, not
# -lpthread, because on FreeBSD only the driver flag selects libthr. Windows
# resolves against the CRT.
set(JITTER_SYSTEM_LIBRARIES "")
if(INTERNAL_TIMER AND NOT ANDROID AND NOT WIN32)
    set(JITTER_SYSTEM_LIBRARIES -pthread)

    # And once more for pkg-config consumers of the static library, for the
    # same reason -lbcrypt is added to this variable on Windows: the exported
    # CMake target carries the dependency as $<LINK_ONLY:pthread>, while the
    # .pc file inherits nothing. The omission stays invisible wherever the
    # pthread symbols live in the C library - glibc 2.34 and later, Apple's
    # libSystem, bionic - and surfaces on everything older, which is most of
    # the enterprise distributions still in support.
    if(NOT BUILD_SHARED_LIBS)
        set(JITTER_PC_LIBS_PRIVATE "${JITTER_PC_LIBS_PRIVATE} -pthread")
    endif()
endif()

# clock_gettime(), which the generic timer back-end calls, is in librt rather
# than the C library on glibc before 2.17 and on Solaris 10; the Makefile links
# -lrt on Linux and SunOS for it. Probed rather than keyed on the system, so
# that a C library that has it links nothing more. CheckFunctionExists links a
# call through a declaration of its own, independent of the feature test macros
# the headers want.
if(NOT WIN32)
    include(CheckFunctionExists)
    check_function_exists(clock_gettime JENT_CLOCK_GETTIME_IN_LIBC)
    if(NOT JENT_CLOCK_GETTIME_IN_LIBC)
        include(CheckLibraryExists)
        check_library_exists(rt clock_gettime "" JENT_CLOCK_GETTIME_IN_LIBRT)
        if(JENT_CLOCK_GETTIME_IN_LIBRT)
            target_link_libraries(${PROJECT_NAME} PRIVATE rt)
            list(APPEND JITTER_SYSTEM_LIBRARIES rt)
            if(NOT BUILD_SHARED_LIBS)
                set(JITTER_PC_LIBS_PRIVATE "${JITTER_PC_LIBS_PRIVATE} -lrt")
            endif()
        endif()
    endif()
endif()

# Two independent options: ENABLE_TOOLS decides what is installed for a user to
# run, BUILD_TESTING what CTest is given. They overlap in gcd,
# jitterentropy-health, jitterentropy-rng and jitterentropy-flags, which the
# suite drives and a user also runs: those are built for either and installed
# only for the first.
#
# BUILD_TESTING is the conventional name packaging already passes, and
# honouring it is safe only because the tools have an option of their own.
# Defined here, not by include(CTest), which drags in the CDash machinery.
if(BUILD_TESTING)
    # Before the subdirectories, so their add_test() calls register.
    enable_testing()
endif()

# libjitterentropy-record, under tests/ so that nothing reaches it by
# accident; see tests/raw-entropy/recording_library/CMakeLists.txt. After the
# library target is complete, as it is copied from it, and after
# enable_testing(), for its test.
if(ENABLE_RECORDING)
    add_subdirectory(tests/raw-entropy/recording_library)
endif()

# The installed tools find a shared library through an rpath relative to
# themselves, $ORIGIN/../lib, so an install under any prefix - and one moved
# elsewhere as a whole - runs without LD_LIBRARY_PATH. Only where the library
# is not in a directory the linker searches anyway: an rpath there is dead
# weight that rpmlint and Lintian report. Decided for the configured prefix:
# configured for a linker directory such as /usr, an install moved elsewhere
# by "cmake --install --prefix" needs -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib'. Set before the subdirectories, whose
# targets take it at creation; a CMAKE_INSTALL_RPATH or CMAKE_SKIP_INSTALL_RPATH
# given on the command line wins. macOS needs none - the install name above is
# absolute - and neither do Windows, Cygwin and MSYS, which find DLLs on PATH.
if(BUILD_SHARED_LIBS AND NOT APPLE AND NOT WIN32 AND NOT CYGWIN AND NOT MSYS
   AND NOT DEFINED CMAKE_INSTALL_RPATH)
    list(FIND CMAKE_C_IMPLICIT_LINK_DIRECTORIES "${CMAKE_INSTALL_FULL_LIBDIR}"
         _jent_libdir_implicit)
    if(_jent_libdir_implicit EQUAL -1)
        file(RELATIVE_PATH _jent_bin_to_lib "${CMAKE_INSTALL_FULL_BINDIR}"
             "${CMAKE_INSTALL_FULL_LIBDIR}")
        set(CMAKE_INSTALL_RPATH "$ORIGIN/${_jent_bin_to_lib}")
    endif()
endif()

# libjitterentropy-kernel and the jitter_rng tool. After the rpath above,
# which the tool takes; see linux_kernel/userspace/CMakeLists.txt.
if(ENABLE_KERNEL_LIB)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "ENABLE_KERNEL_LIB needs Linux: it wraps the interfaces of the "
            "Linux kernel module")
    endif()
    add_subdirectory(linux_kernel/userspace)
endif()

# jitterentropy-rngd and its systemd unit; see rngd/CMakeLists.txt.
if(ENABLE_RNGD)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "ENABLE_RNGD needs Linux: the daemon feeds the Linux /dev/random")
    endif()
    add_subdirectory(rngd)
endif()

# The three the suite drives, plus the rest of their directories.
if(ENABLE_TOOLS OR BUILD_TESTING)
    add_subdirectory(tests/gcd)
    add_subdirectory(tests/health)
    add_subdirectory(tests/raw-entropy/recording_userspace)
endif()

# Test programs only: nothing here is installed.
if(BUILD_TESTING)
    add_subdirectory(tests/unit)
    add_subdirectory(tests/fuzz)
endif()

# Assert the export set of the built shared library against version.lds,
# which configure already checked against jitterentropy.h. The version script
# is not applied on macOS or Windows, where JENT_PRIVATE_STATIC on an internal
# declaration would export it. nm needs -D for ELF and must not get it for
# Mach-O. PE (Windows, Cygwin, MSYS) is skipped: it exports from the same
# declarations, so a leak shows on macOS as well.
if(BUILD_TESTING AND BUILD_SHARED_LIBS AND NOT WIN32 AND NOT CYGWIN AND NOT MSYS)
    find_program(JENT_NM NAMES nm llvm-nm)
    if(JENT_NM)
        if(APPLE)
            set(JENT_NM_ARGS -g)
        else()
            set(JENT_NM_ARGS -D -g)
        endif()
        add_test(NAME exported-symbols
                 COMMAND ${CMAKE_COMMAND}
                         -DJENT_LIB=$<TARGET_FILE:${PROJECT_NAME}>
                         -DJENT_VERSION_SCRIPT=${JITTER_VERSION_SCRIPT}
                         -DJENT_NM=${JENT_NM}
                         "-DJENT_NM_ARGS=${JENT_NM_ARGS}"
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/JentCheckExports.cmake)
        # An nm output format the check cannot read is a skip.
        set_tests_properties(exported-symbols PROPERTIES
            SKIP_REGULAR_EXPRESSION "the export check does not know the output format")

        # The library's exports and the recording's, nothing of the
        # internals the recording reaches.
        if(ENABLE_RECORDING)
            add_test(NAME exported-symbols-record
                     COMMAND ${CMAKE_COMMAND}
                             -DJENT_LIB=$<TARGET_FILE:${PROJECT_NAME}-record>
                             -DJENT_VERSION_SCRIPT=${JITTER_RECORD_VERSION_SCRIPT}
                             -DJENT_NM=${JENT_NM}
                             "-DJENT_NM_ARGS=${JENT_NM_ARGS}"
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/JentCheckExports.cmake)
            set_tests_properties(exported-symbols-record PROPERTIES
                SKIP_REGULAR_EXPRESSION "the export check does not know the output format")
        endif()
        if(ENABLE_KERNEL_LIB)
            add_test(NAME exported-symbols-kernel
                     COMMAND ${CMAKE_COMMAND}
                             -DJENT_LIB=$<TARGET_FILE:${PROJECT_NAME}-kernel>
                             -DJENT_VERSION_SCRIPT=${JITTER_KERNEL_VERSION_SCRIPT}
                             -DJENT_NM=${JENT_NM}
                             "-DJENT_NM_ARGS=${JENT_NM_ARGS}"
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/JentCheckExports.cmake)
            set_tests_properties(exported-symbols-kernel PROPERTIES
                SKIP_REGULAR_EXPRESSION "the export check does not know the output format")
        endif()
    else()
        message(STATUS
            "no nm found, not checking the export set of the shared library")
    endif()
endif()

# Tools only: nothing here is driven by the suite.
if(ENABLE_TOOLS)
    add_subdirectory(tests/raw-entropy/validation-runtime)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Both reach <linux/ioctl.h> through the module's UAPI header.
        add_subdirectory(tests/raw-entropy/recording_runtime_kernelspace)
        add_subdirectory(tests/chardev)
    endif()
endif()

# Here rather than in the subdirectory building them: what they are asked to do
# is a property of the build, not the tool.
#
# The GCD self test is deterministic and always gated on. The generation runs
# reach the real noise source and can fail on properties of the machine, so
# they carry the "unreliable" label a gating run excludes.
if(BUILD_TESTING)
    add_test(NAME gcd-self-test COMMAND gcd)

    # The bare run, separate from the loop below: an empty element does not
    # survive a CMake list.
    add_test(NAME rng-default COMMAND jitterentropy-rng 256)
    set_tests_properties(rng-default PROPERTIES LABELS unreliable)

    # --force-internal-timer only where there is one to force: without
    # JENT_CONF_ENABLE_INTERNAL_TIMER the initialization refuses the flag, so
    # registering it in that build would make the suite fail on a supported
    # configuration rather than on a defect.
    set(_rng_modes "--all-caches" "--force-fips" "--ntg1")
    if(INTERNAL_TIMER)
        list(APPEND _rng_modes "--force-internal-timer")
    endif()

    foreach(_mode IN LISTS _rng_modes)
        string(REGEX REPLACE "^--" "" _name "${_mode}")
        add_test(NAME rng-${_name} COMMAND jitterentropy-rng 256 ${_mode})
        set_tests_properties(rng-${_name} PROPERTIES LABELS unreliable)
    endforeach()

    # jitterentropy-rng links the library, and a Windows shared build leaves
    # the DLL in the top-level output directory while the tool is built into
    # its own. Nothing puts the two together at run time - there is no rpath
    # on Windows and the loader does not search the linking target's
    # directory - so every rng-* test dies with STATUS_DLL_NOT_FOUND
    # (0xc0000135) before reaching main(). Put the DLL on their PATH.
    #
    # path_list_prepend prepends to the inherited PATH using the separator of
    # the platform, so neither the host PATH nor the semicolons in it are
    # named here - which is what the CMake 3.22 floor buys: spelled with the
    # plain ENVIRONMENT property this has to interpolate $ENV{PATH} and
    # escape every separator in it against the list the property itself is.
    if(WIN32 AND BUILD_SHARED_LIBS)
        set(_rng_tests rng-default)
        foreach(_mode IN LISTS _rng_modes)
            string(REGEX REPLACE "^--" "" _name "${_mode}")
            list(APPEND _rng_tests rng-${_name})
        endforeach()
        set_tests_properties(${_rng_tests} PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:${PROJECT_NAME}>")
    endif()
endif()

# gcovr over lcov: it reads either compiler's counters, needs no capture step
# and writes the HTML itself; lcov + genhtml where it is absent. tests/ is
# excluded - test programs counting as covered code hide the one thing of
# interest, which lines of the library the tests reach.
if(ENABLE_COVERAGE)
    find_program(GCOVR_EXECUTABLE gcovr)
    find_program(LCOV_EXECUTABLE lcov)
    find_program(GENHTML_EXECUTABLE genhtml)

    set(JITTER_COVERAGE_DIR ${PROJECT_BINARY_DIR}/coverage)

    if(GCOVR_EXECUTABLE)
        # gcc's gcov cannot parse clang's counters, and the bare name does not
        # reach llvm-cov: an Apple toolchain keeps it off PATH, where only
        # xcrun finds it. Look beside the compiler, then ask xcrun.
        if(CLANG)
            get_filename_component(_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
            find_program(LLVM_COV_EXECUTABLE NAMES llvm-cov HINTS "${_cc_dir}")
            if(NOT LLVM_COV_EXECUTABLE AND APPLE)
                find_program(XCRUN_EXECUTABLE xcrun)
                if(XCRUN_EXECUTABLE)
                    execute_process(
                        COMMAND ${XCRUN_EXECUTABLE} --find llvm-cov
                        RESULT_VARIABLE _xcrun_result
                        OUTPUT_VARIABLE _xcrun_llvm_cov
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
                    if(_xcrun_result EQUAL 0 AND EXISTS "${_xcrun_llvm_cov}")
                        set(LLVM_COV_EXECUTABLE "${_xcrun_llvm_cov}"
                            CACHE FILEPATH "llvm-cov used to read the coverage counters" FORCE)
                    endif()
                endif()
            endif()
            if(NOT LLVM_COV_EXECUTABLE)
                message(FATAL_ERROR
                    "ENABLE_COVERAGE with ${CMAKE_C_COMPILER_ID} needs llvm-cov, "
                    "which reads the counters this compiler writes")
            endif()
            set(_gcov_tool --gcov-executable "${LLVM_COV_EXECUTABLE} gcov")
        else()
            set(_gcov_tool "")
        endif()
        # merge-mode-functions because the same source is compiled more than
        # once here, one copy with the mocked time source: gcov keys a function
        # on its first executable line, which a conditional block moves, and
        # the default "strict" merge then refuses to combine them.
        #
        # The suspicious hit counts must be tolerated: the internal timer spins
        # a second thread on a counter that gcov counts non-atomically, and the
        # nonsensical result is a fatal parse error unless downgraded.
        #
        # The narrow value postdates gcovr 7.0, newer than Ubuntu 24.04 ships;
        # "all" also swallows errors worth failing on. Probed by running gcovr
        # rather than by version arithmetic.
        execute_process(
            COMMAND ${GCOVR_EXECUTABLE}
                    --gcov-ignore-parse-errors suspicious_hits.warn --version
            RESULT_VARIABLE _gcovr_suspicious_hits
            OUTPUT_QUIET ERROR_QUIET)
        if(_gcovr_suspicious_hits EQUAL 0)
            set(_gcov_parse_errors suspicious_hits.warn)
        else()
            set(_gcov_parse_errors all)
            message(STATUS
                "gcovr does not accept --gcov-ignore-parse-errors "
                "suspicious_hits.warn, falling back to all")
        endif()
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory ${JITTER_COVERAGE_DIR}
            COMMAND ${CMAKE_COMMAND}
                    -DCTEST_EXECUTABLE=${CMAKE_CTEST_COMMAND}
                    -P ${PROJECT_SOURCE_DIR}/cmake/RunTestsForCoverage.cmake
            COMMAND ${GCOVR_EXECUTABLE}
                    --root ${PROJECT_SOURCE_DIR}
                    ${_gcov_tool}
                    --gcov-ignore-parse-errors ${_gcov_parse_errors}
                    --merge-mode-functions merge-use-line-min
                    --exclude "${PROJECT_SOURCE_DIR}/tests/"
                    --exclude "${PROJECT_SOURCE_DIR}/linux_kernel/"
                    --html-details ${JITTER_COVERAGE_DIR}/index.html
                    --print-summary
            COMMENT "Running the test suite and writing the HTML coverage report"
            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
            VERBATIM)
    elseif(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
        add_custom_target(coverage
            COMMAND ${LCOV_EXECUTABLE} --directory ${PROJECT_BINARY_DIR} --zerocounters -q
            COMMAND ${CMAKE_COMMAND}
                    -DCTEST_EXECUTABLE=${CMAKE_CTEST_COMMAND}
                    -P ${PROJECT_SOURCE_DIR}/cmake/RunTestsForCoverage.cmake
            COMMAND ${LCOV_EXECUTABLE} --directory ${PROJECT_BINARY_DIR}
                    --capture --output-file coverage.info -q
            COMMAND ${LCOV_EXECUTABLE} --remove coverage.info
                    "${PROJECT_SOURCE_DIR}/tests/*"
                    "${PROJECT_SOURCE_DIR}/linux_kernel/*" "/usr/*"
                    --output-file coverage.info -q
            COMMAND ${GENHTML_EXECUTABLE} coverage.info
                    --output-directory ${JITTER_COVERAGE_DIR}
            COMMENT "Running the test suite and writing the HTML coverage report"
            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
            VERBATIM)
    else()
        message(FATAL_ERROR
            "ENABLE_COVERAGE needs either gcovr or lcov together with genhtml")
    endif()

    message(STATUS
        "Coverage report target enabled, HTML is written to ${JITTER_COVERAGE_DIR}")
endif()

# pkg-config
# The private dependencies are appended with a leading blank each.
string(STRIP "${JITTER_PC_LIBS_PRIVATE}" JITTER_PC_LIBS_PRIVATE)
#
# libdir and includedir are spelled relative to ${prefix} where GNUInstallDirs
# has them relative, and prefix itself is only filled in by the install: a
# "cmake --install --prefix" changes CMAKE_INSTALL_PREFIX then, and a prefix
# written at configure time pointed the installed file at the old tree. Not
# ${pcfiledir}, which pkg-config prefixes with PKG_CONFIG_SYSROOT_DIR a second
# time in a cross build. The build tree's jitterentropy.pc keeps the configure
# prefix.
#
# pkg-config splits Libs and Cflags at blanks after expanding the variables,
# so a prefix of "C:/Program Files (x86)/jitterentropy" gave -LC:/Program and
# a stray Files... argument. A blank in any path written here is escaped with
# a backslash, which pkg-config and pkgconf both take out again - as they
# would a backslash separator, hence forward slashes only.
foreach(_jent_pc_dir LIBDIR INCLUDEDIR)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_${_jent_pc_dir}}")
        string(REPLACE " " "\\ " JENT_PC_${_jent_pc_dir}
               "${CMAKE_INSTALL_${_jent_pc_dir}}")
    else()
        set(JENT_PC_${_jent_pc_dir} "\${prefix}/${CMAKE_INSTALL_${_jent_pc_dir}}")
    endif()
endforeach()
string(REPLACE " " "\\ " JENT_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/jitterentropy.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropy.pc
  @ONLY
)
# The same once more with the placeholder left in, for the install to fill.
set(JENT_PC_PREFIX "@JENT_PC_PREFIX@")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/jitterentropy.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropy.pc.install
  @ONLY
)
set(_jent_pc_installed ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/jitterentropy.pc)
install(CODE "
  set(_jent_pc_in \"${CMAKE_CURRENT_BINARY_DIR}/jitterentropy.pc.install\")
  set(_jent_pc_out \"${_jent_pc_installed}\")
")
install(CODE [[
  set(_jent_pc_prefix "${CMAKE_INSTALL_PREFIX}")
  if(CMAKE_HOST_WIN32)
    string(REPLACE "\\" "/" _jent_pc_prefix "${_jent_pc_prefix}")
  endif()
  string(REPLACE " " "\\ " _jent_pc_prefix "${_jent_pc_prefix}")
  file(READ "${_jent_pc_in}" _jent_pc)
  string(REPLACE "@JENT_PC_PREFIX@" "${_jent_pc_prefix}" _jent_pc "${_jent_pc}")
  file(WRITE "${_jent_pc_out}" "${_jent_pc}")
]])
install(FILES
  ${_jent_pc_installed}
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# cmake
#
# Same destination as the targets file above - the config file includes it by
# CMAKE_CURRENT_LIST_DIR, so the two have to stay together.
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/jitterentropyConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropyConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropyConfigVersion.cmake
  VERSION       ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropyConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropyConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

# man pages, for a target that can display them - see INSTALL_MAN above: the
# overview, doc/jitterentropy.3, a page per group of functions, the tools'
# where they are installed, and the kernel module's on Linux. Every title
# names the version as x.y.z and the overview's synopsis as x, y and z; the
# installed pages carry the values of this build. A placeholder that is gone
# fails the configure rather than installing a page without the version.
if(INSTALL_MAN)
    set(_jent_man_pages
        jitterentropy.3
        jent_entropy_collector_alloc.3
        jent_entropy_init.3
        jent_entropy_switch_notime_impl.3
        jent_read_entropy.3
        jent_set_fips_failure_callback.3
        jent_status.3)
    if(ENABLE_TOOLS)
        list(APPEND _jent_man_pages jitterentropy-flags.1
             jitterentropy-hashtime.1 jitterentropy-osr.1 jitterentropy-rng.1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND _jent_man_pages jitter_rng.4)
    endif()
    if(ENABLE_KERNEL_LIB)
        list(APPEND _jent_man_pages jitterentropy-kernel.3 jitter_rng.1)
    endif()
    if(ENABLE_RECORDING)
        list(APPEND _jent_man_pages jitterentropy-record.3)
    endif()
    if(ENABLE_RNGD)
        list(APPEND _jent_man_pages jitterentropy-rngd.1)
    endif()
    foreach(_jent_man_file ${_jent_man_pages})
        string(REGEX REPLACE "\\.([0-9])$" ";\\1" _jent_man_pair "${_jent_man_file}")
        list(GET _jent_man_pair 0 _jent_man_page)
        list(GET _jent_man_pair 1 _jent_man_sec)
        set(_jent_man_installed)
        set(_jent_man_src ${CMAKE_CURRENT_SOURCE_DIR}/doc/${_jent_man_file})
        set(_jent_man_out ${CMAKE_CURRENT_BINARY_DIR}/${_jent_man_file})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     ${_jent_man_src})
        file(READ ${_jent_man_src} _jent_man)

        set(_jent_man_from "\"jitterentropy x.y.z\"")
        set(_jent_man_to "\"jitterentropy ${PROJECT_VERSION}\"")
        if(_jent_man_page STREQUAL "jitterentropy")
            foreach(_jent_man_macro MAJVERSION:x MINVERSION:y PATCHLEVEL:z)
                string(REPLACE ":" ";" _jent_man_pair "${_jent_man_macro}")
                list(GET _jent_man_pair 0 _jent_man_name)
                list(GET _jent_man_pair 1 _jent_man_placeholder)
                list(APPEND _jent_man_from
                     "#define JENT_${_jent_man_name} ${_jent_man_placeholder}\"")
                list(APPEND _jent_man_to
                     "#define JENT_${_jent_man_name} ${JENT_${_jent_man_name}}\"")
            endforeach()
        endif()
        foreach(_jent_man_f _jent_man_t IN ZIP_LISTS _jent_man_from _jent_man_to)
            string(FIND "${_jent_man}" "${_jent_man_f}" _jent_man_at)
            if(_jent_man_at EQUAL -1)
                message(FATAL_ERROR
                        "doc/${_jent_man_file} lacks '${_jent_man_f}'")
            endif()
            string(REPLACE "${_jent_man_f}" "${_jent_man_t}"
                   _jent_man "${_jent_man}")
        endforeach()
        # Removed first: a build directory from before the split has this name
        # as a symlink to jitterentropy.3, and writing would go through it.
        file(REMOVE ${_jent_man_out})
        file(WRITE ${_jent_man_out} "${_jent_man}")
        list(APPEND _jent_man_installed ${_jent_man_out})

        # An alias per further function the NAME section lists, so that
        # "man jent_read_entropy_safe" finds its page. Symlinks rather than .so
        # pages: man opens a link given as a path too, while it resolves a .so
        # against the current directory then.
        if(NOT _jent_man MATCHES "\\.SH NAME\n([^\n]*) \\\\- ")
            message(FATAL_ERROR "doc/${_jent_man_file} has no NAME line")
        endif()
        string(REPLACE ", " ";" _jent_man_names "${CMAKE_MATCH_1}")
        list(REMOVE_ITEM _jent_man_names ${_jent_man_page})
        # Made here and installed with install(FILES), which copies a symlink as
        # one, so that install_manifest.txt lists them. The link is made on the
        # build host, though, and a Windows host - a cross build for Linux, or
        # INSTALL_MAN forced on - needs a privilege to create a symlink and to
        # install one, so there the alias is a copy of the page instead.
        foreach(_jent_man_alias ${_jent_man_names})
            set(_jent_man_link
                ${CMAKE_CURRENT_BINARY_DIR}/${_jent_man_alias}.${_jent_man_sec})
            if(CMAKE_HOST_WIN32)
                file(COPY_FILE ${_jent_man_out} ${_jent_man_link}
                     ONLY_IF_DIFFERENT)
            else()
                file(CREATE_LINK ${_jent_man_file} ${_jent_man_link} SYMBOLIC)
            endif()
            list(APPEND _jent_man_installed ${_jent_man_link})
        endforeach()
        install(FILES ${_jent_man_installed}
                DESTINATION ${CMAKE_INSTALL_MANDIR}/man${_jent_man_sec})
    endforeach()
endif()
