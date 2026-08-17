cmake_minimum_required(VERSION 3.20)

# From jitterentropy.h, as the Makefile does, so the package files cannot
# drift from the library version.
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

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(STACK_PROTECTOR "Compile Jitter with stack protector enabled" ON)
option(INTERNAL_TIMER "Compile Jitter with the internal thread based timer" ON)
option(EXTERNAL_CRYPTO "Compile Jitter and use an external libcrypto, valid options are [AWSLC, OPENSSL, LIBGCRYPT]")
option(AARCH64_NSTIME_REGISTER "Name of the register that should be used in jent_get_nstime function")
option(ENABLE_SANITIZERS "Compile with address and undefined behavior sanitizers (only for development)" OFF)
option(ENABLE_COVERAGE "Instrument the build for code coverage and add the 'coverage' target (only for development)" OFF)
option(MOCK_TIMER "Allow the time source to be replaced by a caller-supplied callback (only for testing - a build with this on produces no entropy of its own)" OFF)
option(ENABLE_TOOLS "Build and install the recording and validation tools (jitterentropy-rng, getrawentropy, extractlsb, ...), which live under tests/" ON)
option(BUILD_TESTING "Build the test programs and register the CTest suite" ON)

# CMake sets MSVC itself; do the same for the others.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(CLANG 1)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(GCC 1)
elseif(CMAKE_C_COMPILER_ID MATCHES "SunPro")
    set(SUNPRO 1)
endif()

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

if(BUILD_SHARED_LIBS)
  set(JITTER_PC_LIBS "-l${CMAKE_PROJECT_NAME}")
  set(JITTER_PC_LIBS_PRIVATE "")
else()
  set(JITTER_PC_LIBS "")
  set(JITTER_PC_LIBS_PRIVATE "-l${CMAKE_PROJECT_NAME}")
endif()

# Appended to the Cflags line of the .pc file. Only the Windows static build
# has anything to put here; see the JENT_STATIC_LIB branch further down.
set(JITTER_PC_CFLAGS "")

if(WIN32)
    if(MSVC)
        # MSVC deprecates the portable spellings for its own _s variants.
        list(APPEND JITTER_C_FLAGS  /Od /W4 /D_CRT_SECURE_NO_WARNINGS)
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
    if((CMAKE_SYSTEM_NAME STREQUAL "FreeBSD") OR (CMAKE_SYSTEM_NAME STREQUAL "OpenBSD") OR (CMAKE_SYSTEM_NAME STREQUAL "NetBSD"))
        list(APPEND JITTER_LINKER_FLAGS -pthread)
    endif()
    if(SUNPRO)
        list(APPEND JITTER_C_FLAGS  -m64 -Kpic -errtags=yes -erroff=E_KW_IS_AN_EXTENSION_OF_ANSI,E_ASM_UNUSED_PARAM)
    else()
        list(APPEND JITTER_C_FLAGS  -fwrapv --param ssp-buffer-size=4 -fPIE -fPIC)
    endif()

    if ((NOT GCC) OR (GCC AND CMAKE_C_COMPILER_VERSION VERSION_GREATER "4.3"))
        # Before GCC 4.3 this meant something else entirely.
        # https://gcc.gnu.org/wiki/NewWconversion.
        set(JITTER_COMPILE_FLAGS "${JITTER_COMPILE_FLAGS} -Wconversion")
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
            # it, tests/ included.
            add_link_options(${JITTER_SSP_FLAG})

            # Kept for the .pc file only; the link itself is handled above.
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
target_sources(${PROJECT_NAME} PRIVATE ${JITTER_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES
    PUBLIC_HEADER "jitterentropy.h"
    # As the Makefile: .so.<maj>.<min>.<patch>, SONAME .so.<maj>.
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_options(${PROJECT_NAME} PRIVATE ${JITTER_C_FLAGS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${JITTER_LINKER_FLAGS})

# version.lds is the exported set of a shared build. Only where the linker
# takes a GNU-style version script - ld64 and the Windows toolchains want their
# own - so it is probed, as the stack protector above is.
set(JITTER_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/version.lds")
if(BUILD_SHARED_LIBS AND NOT WIN32 AND NOT APPLE)
    include(CheckLinkerFlag)
    check_linker_flag(C "-Wl,--version-script=${JITTER_VERSION_SCRIPT}"
                      JITTER_VERSION_SCRIPT_USABLE)
    if(JITTER_VERSION_SCRIPT_USABLE)
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
# block() is CMake 3.25 and this project asks for 3.20.
function(jent_check_version_script header script linktest)
    set(header_api "")
    set(take_next FALSE)
    file(STRINGS "${header}" lines)
    foreach(line IN LISTS lines)
        if(take_next)
            if(line MATCHES "(jent_[A-Za-z0-9_]+)\\(")
                list(APPEND header_api "${CMAKE_MATCH_1}")
            endif()
            set(take_next FALSE)
        elseif(line MATCHES "^JENT_PRIVATE_STATIC$")
            set(take_next TRUE)
        endif()
    endforeach()

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
            "version.lds exports the ${n} functions of jitterentropy.h")
        jent_check_linktest_calls("${linktest}" "${header_api}")
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
        "version.lds and jitterentropy.h disagree. "
        "Only in the header: ${only_header}. "
        "Only in the script: ${only_script}.")
endfunction()

jent_check_version_script("${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy.h"
                          "${JITTER_VERSION_SCRIPT}"
                          "${CMAKE_CURRENT_SOURCE_DIR}/tests/linking/jitterentropy-linktest.c")

# As bcrypt below: a static consumer links this archive's instrumented frames
# and must resolve the symbols itself. The exported CMake target says so, the
# .pc file does not.
if(JITTER_SSP_LINK_FLAGS AND NOT BUILD_SHARED_LIBS)
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

if(EXTERNAL_CRYPTO STREQUAL "AWSLC")
    # Unique to AWS-LC.
    set(EXTERNAL_CRYPTO_HEADER "openssl/base.h")
    set(EXTERNAL_CRYPTO_LIBRARY_NAME libcrypto)
elseif(EXTERNAL_CRYPTO STREQUAL "OPENSSL")
    set(EXTERNAL_CRYPTO_HEADER "openssl/crypto.h")
    set(EXTERNAL_CRYPTO_LIBRARY_NAME libcrypto)
elseif(EXTERNAL_CRYPTO STREQUAL "LIBGCRYPT")
    set(EXTERNAL_CRYPTO_HEADER "gcrypt.h")
    set(EXTERNAL_CRYPTO_LIBRARY_NAME libgcrypt)
elseif(NOT EXTERNAL_CRYPTO)
    message(STATUS "Using internal functions for everything")
else()
    message(FATAL_ERROR "Unknown EXTERNAL_CRYPTO option ${EXTERNAL_CRYPTO}")
endif()

if(EXTERNAL_CRYPTO)
    find_path(LIBCRYPTO_INCLUDE_DIR
            NAMES "${EXTERNAL_CRYPTO_HEADER}"
            PATH_SUFFIXES include
            )
    message(STATUS "Found external crypto headers at ${LIBCRYPTO_INCLUDE_DIR}")
    target_include_directories(${PROJECT_NAME} PUBLIC ${LIBCRYPTO_INCLUDE_DIR})
    if(BUILD_SHARED_LIBS)
        find_library(LIBCRYPTO_LIBRARY
                NAMES "${EXTERNAL_CRYPTO_LIBRARY_NAME}.so" "${EXTERNAL_CRYPTO_LIBRARY_NAME}.dylib" "${EXTERNAL_CRYPTO_LIBRARY_NAME}.dll"
                HINTS "${CMAKE_INSTALL_PREFIX}"
                PATH_SUFFIXES build/crypto build lib64 lib
                )
        set(JITTER_PC_REQUIRES "${EXTERNAL_CRYPTO_LIBRARY_NAME}")
    else()
        find_library(LIBCRYPTO_LIBRARY
                NAMES "${EXTERNAL_CRYPTO_LIBRARY_NAME}.a" "${EXTERNAL_CRYPTO_LIBRARY_NAME}.lib"
                HINTS "${CMAKE_INSTALL_PREFIX}"
                PATH_SUFFIXES build/crypto build lib64 lib
                )
        set(JITTER_PC_REQUIRES_PRIVATE "${EXTERNAL_CRYPTO_LIBRARY_NAME}")
    endif()
    message(STATUS "Found external crypto library ${LIBCRYPTO_LIBRARY}")
    target_link_libraries(${PROJECT_NAME} PUBLIC ${LIBCRYPTO_LIBRARY})
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
# pthread_create() themselves. -pthread, not -lpthread, because on FreeBSD only
# the driver flag selects libthr. Windows resolves against the CRT.
set(JITTER_THREAD_LIBRARIES "")
if(INTERNAL_TIMER AND NOT ANDROID AND NOT WIN32)
    set(JITTER_THREAD_LIBRARIES -pthread)

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

# Two independent options: ENABLE_TOOLS decides what is installed for a user to
# run, BUILD_TESTING what CTest is given. They overlap in gcd,
# jitterentropy-health and jitterentropy-rng, which the suite drives and a user
# also runs: those are built for either and installed only for the first.
#
# BUILD_TESTING is the conventional name packaging already passes, and
# honouring it is safe only because the tools have an option of their own.
# Defined here, not by include(CTest), which drags in the CDash machinery.
if(BUILD_TESTING)
    # Before the subdirectories, so their add_test() calls register.
    enable_testing()
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

    foreach(_mode "" "--all-caches" "--force-internal-timer" "--force-fips" "--ntg1")
        if(_mode STREQUAL "")
            set(_name default)
            set(_args "")
        else()
            string(REGEX REPLACE "^--" "" _name "${_mode}")
            set(_args "${_mode}")
        endif()
        add_test(NAME rng-${_name} COMMAND jitterentropy-rng 256 ${_args})
        set_tests_properties(rng-${_name} PROPERTIES LABELS unreliable)
    endforeach()
endif()

# gcovr over lcov: it reads either compiler's counters, needs no capture step
# and writes the HTML itself; lcov + genhtml where it is absent. tests/ is
# excluded - test programs counting as covered code hide the one thing of
# interest, which lines of the library the tests reach.
if(ENABLE_COVERAGE)
    find_program(GCOVR_EXECUTABLE gcovr)
    find_program(LCOV_EXECUTABLE lcov)
    find_program(GENHTML_EXECUTABLE genhtml)

    set(JITTER_COVERAGE_DIR ${CMAKE_BINARY_DIR}/coverage)

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
                    -P ${CMAKE_SOURCE_DIR}/cmake/RunTestsForCoverage.cmake
            COMMAND ${GCOVR_EXECUTABLE}
                    --root ${CMAKE_SOURCE_DIR}
                    ${_gcov_tool}
                    --gcov-ignore-parse-errors ${_gcov_parse_errors}
                    --merge-mode-functions merge-use-line-min
                    --exclude "${CMAKE_SOURCE_DIR}/tests/"
                    --exclude "${CMAKE_SOURCE_DIR}/linux_kernel/"
                    --html-details ${JITTER_COVERAGE_DIR}/index.html
                    --print-summary
            COMMENT "Running the test suite and writing the HTML coverage report"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            VERBATIM)
    elseif(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
        add_custom_target(coverage
            COMMAND ${LCOV_EXECUTABLE} --directory ${CMAKE_BINARY_DIR} --zerocounters -q
            COMMAND ${CMAKE_COMMAND}
                    -DCTEST_EXECUTABLE=${CMAKE_CTEST_COMMAND}
                    -P ${CMAKE_SOURCE_DIR}/cmake/RunTestsForCoverage.cmake
            COMMAND ${LCOV_EXECUTABLE} --directory ${CMAKE_BINARY_DIR}
                    --capture --output-file coverage.info -q
            COMMAND ${LCOV_EXECUTABLE} --remove coverage.info
                    "${CMAKE_SOURCE_DIR}/tests/*"
                    "${CMAKE_SOURCE_DIR}/linux_kernel/*" "/usr/*"
                    --output-file coverage.info -q
            COMMAND ${GENHTML_EXECUTABLE} coverage.info
                    --output-directory ${JITTER_COVERAGE_DIR}
            COMMENT "Running the test suite and writing the HTML coverage report"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            VERBATIM)
    else()
        message(FATAL_ERROR
            "ENABLE_COVERAGE needs either gcovr or lcov together with genhtml")
    endif()

    message(STATUS
        "Coverage report target enabled, HTML is written to ${JITTER_COVERAGE_DIR}")
endif()

# pkg-config
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/jitterentropy.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropy.pc
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropy.pc
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

# man page(s)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/doc/jitterentropy.3
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man3)
