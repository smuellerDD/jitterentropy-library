cmake_minimum_required(VERSION 3.20)

# Derive the version from jitterentropy.h (the single source of truth, as the
# Makefile does) so the pkg-config / CMake package files cannot drift from the
# library version.
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

# CMake defines the variable MSVC to true automatically when building with MSVC, replicate that for other compilers
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

if(BUILD_SHARED_LIBS)
  set(JITTER_PC_LIBS "-l${CMAKE_PROJECT_NAME}")
  set(JITTER_PC_LIBS_PRIVATE "")
else()
  set(JITTER_PC_LIBS "")
  set(JITTER_PC_LIBS_PRIVATE "-l${CMAKE_PROJECT_NAME}")
endif()

if(WIN32)
    if(MSVC)
        # _CRT_SECURE_NO_WARNINGS: MSVC deprecates the standard C library in
        # favour of its own _s variants (C4996 on fopen, strtok, ...). The
        # code has to stay portable, so the "insecure" spellings are the
        # intended ones and the advice is noise.
        list(APPEND JITTER_C_FLAGS  /Od /W4 /D_CRT_SECURE_NO_WARNINGS)
    else()
        list(APPEND JITTER_C_FLAGS  -fwrapv --param ssp-buffer-size=4 -fvisibility=hidden -Wcast-align
        -Wmissing-field-initializers -Wshadow -Wswitch-enum -Wextra -Wall -pedantic -O0 -fwrapv -Wconversion)
    endif()
else()
    list(APPEND JITTER_C_FLAGS  -fvisibility=hidden -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum -Wextra -Wall -pedantic -O0 -Wconversion)

    # This library reports failure the POSIX way, as a negated errno - see
    # "Returns 0 on success or a negative errno on failure" in jitterentropy.h
    # and the ~30 "return -EXXX" sites behind it. That convention assumes errno
    # constants are positive, which on Haiku they are not: its <errno.h> maps
    # them onto Haiku's own B_* error codes, which are large negative numbers
    # based at B_GENERAL_ERROR_BASE == INT_MIN.
    #
    # Negating those inverts the contract. ENOENT is B_ENTRY_NOT_FOUND, so
    # -ENOENT comes out as +2147459069 and every "if (ret < 0)" reads that
    # failure as success - silently, because nothing diagnoses it. Only ENOMEM
    # is noticed at all, and only because it is exactly INT_MIN, where negation
    # overflows and GCC says so ("integer overflow in expression
    # '-2147483648'"); it stays negative by accident and so keeps working.
    #
    # B_USE_POSITIVE_POSIX_ERRORS is Haiku's own switch for exactly this: it
    # turns B_TO_POSIX_ERROR() into a negation so that the POSIX names take
    # their usual positive values, and then -EXXX means what it means
    # everywhere else. Preferred over rewriting the return sites because it
    # leaves every other platform untouched and keeps one convention in the
    # source. It has to reach the compiler before any system header, hence a
    # command-line define rather than one in a header of ours.
    if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        list(APPEND JITTER_C_FLAGS -DB_USE_POSITIVE_POSIX_ERRORS)
    endif()

    # -z relro is a GNU ld / lld spelling. The Solaris link editor rejects it
    # outright - "ld: fatal: option -z has illegal argument 'relro'" - while
    # accepting -z now, so Solaris and illumos get the immediate-binding half
    # of the hardening and not the read-only-relocation half.
    #
    # Keyed on the target system rather than on the compiler. The constraint
    # belongs to the linker, and on those systems GCC drives the same Solaris
    # ld that the SunPro compiler does, so testing SUNPRO alone silently missed
    # every GCC build - which is what broke the OpenIndiana job. Apple is
    # excluded for the same class of reason: ld64 understands neither form.
    #
    # The Makefile reaches the same conclusion through its UNAME_S filter,
    # which lists the platforms that get these flags rather than the ones that
    # do not.
    #
    # Cygwin is excluded outright: its ld targets PE/COFF like MinGW's and
    # rejects the option at the first hurdle - "ld: unrecognized option '-z'" -
    # even though CMake does not set WIN32 for it and everything else in this
    # branch applies.
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
        # -Wconversion was changed from GCC version 4.3. Prior it was meant as
        # an aid in translating code from old C to modern C. It was not meant
        # to help detect troublesome implicit conversions.
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
    elseif(CLANG)
        set(JITTER_SSP_FLAG -fstack-protector-strong)
    endif()

    if(JITTER_SSP_FLAG)
        # -fstack-protector* emits references to __stack_chk_fail and
        # __stack_chk_guard, and something has to define them. Most C libraries
        # do (glibc since 2.4, the BSDs, Apple, bionic, musl, Cygwin), so the
        # flag costs nothing extra at link time and every platform built until
        # now hid the question. Solaris' libc defines neither and the runtime
        # has to come from GCC's own libssp instead - without it the link dies
        # with "ld: fatal: symbol referencing errors". Haiku has neither the
        # symbols nor a usable libssp, so there the flag cannot be honoured at
        # all.
        #
        # The flag therefore belongs on the *link* command as well as the
        # compile one. That is how the compiler driver is told to bring the
        # runtime in: GCC on Solaris links libssp from its own spec when it
        # sees -fstack-protector* while driving the link, and adds nothing when
        # it does not. Naming -lssp here instead would hardcode one platform's
        # spelling of a decision the driver already knows how to make.
        #
        # This was originally a compile-only flag, which is exactly why Solaris
        # failed: the objects came out instrumented and the link that consumed
        # them never mentioned the flag, so nothing pulled the runtime in and
        # every executable died on undefined __stack_chk_fail/__stack_chk_guard.
        #
        # A probe rather than a check on CMAKE_SYSTEM_NAME: illumos added both
        # symbols to its libc (illumos issue #5788) and needs no help, yet it
        # reports SunOS exactly as Solaris does, so the system name cannot tell
        # the two apart. The constraint belongs to the toolchain, so the
        # toolchain is what gets asked - the same reasoning as the floor()/-lm
        # probe in tests/raw-entropy/recording_userspace/CMakeLists.txt.
        #
        # check_c_source_compiles() compiles and links in a single driver
        # invocation with the flag on it, which is precisely the arrangement
        # used below, so what the probe answers is what the build will do.
        #
        # The probe needs a function the flag actually instruments:
        # -fstack-protector-strong only guards frames with a local array, so an
        # empty main() would link on any platform and prove nothing.
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

        # Emitted only where the runtime behind it exists. It is hardening, not
        # functionality, and STACK_PROTECTOR is on by default, so a toolchain
        # that cannot supply the runtime must not be left unable to build the
        # library at all - which is what a hard error here would mean for
        # Haiku, whose libroot carries neither symbol and which ships no
        # libssp to fall back on. Dropped with a warning instead, because a
        # security option that quietly turns itself off is worse than one that
        # goes missing loudly.
        if(JITTER_SSP_USABLE)
            list(APPEND JITTER_C_FLAGS ${JITTER_SSP_FLAG})

            # Every target compiled with the flag needs it on its own link
            # command, not just the library. Attaching it to the library target
            # alone is enough for a static build, where CMake replays a
            # static library's PRIVATE link items into its consumers, but a
            # shared library propagates nothing - so the test programs, which
            # compile their own instrumented frames with JITTER_C_FLAGS, lost
            # it exactly there. Solaris then failed them on
            # "__stack_chk_fail ... symbol belongs to implicit dependency
            # libssp.so.0": reaching libssp through libjitterentropy.so is not
            # something its link editor will resolve a direct reference
            # against.
            #
            # add_link_options() applies to every target created after it,
            # including those in the tests/ subdirectories, and mirrors what
            # ENABLE_SANITIZERS does above for -fsanitize, which needs to be on
            # both command lines for the same reason.
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

# CONFIGURE_DEPENDS: re-evaluate the glob on every build so an added/removed
# source file cannot silently produce an archive with missing objects from a
# stale configure.
file(GLOB JITTER_SRC CONFIGURE_DEPENDS "src/*.c" "arch/*.c")
add_library(${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE ${JITTER_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES
    PUBLIC_HEADER "jitterentropy.h"
    # Match the Makefile: libjitterentropy.so.<maj>.<min>.<patch> with
    # SONAME libjitterentropy.so.<maj>.
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_options(${PROJECT_NAME} PRIVATE ${JITTER_C_FLAGS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${JITTER_LINKER_FLAGS})

# The same treatment bcrypt gets below, for the same reason. A static consumer
# links this archive's stack-protected frames into its own program and so has to
# resolve __stack_chk_fail/__stack_chk_guard itself; the exported CMake target
# carries that as $<LINK_ONLY:ssp>, but nothing propagates it into the .pc file.
# A shared build resolves them inside the library and needs nothing here.
if(JITTER_SSP_LINK_FLAGS AND NOT BUILD_SHARED_LIBS)
    set(JITTER_PC_LIBS_PRIVATE "${JITTER_PC_LIBS_PRIVATE} ${JITTER_SSP_LINK_FLAGS}")
endif()

if(WIN32)
    # arch/jitterentropy-arch-uuid.c calls BCryptGenRandom(). MSVC picks the
    # import library up from the #pragma comment(lib, ...) in that file, but
    # that pragma is MSVC-only: a MinGW build has to be told here, and without
    # it every link failed on the undefined symbol. jent_uuid_generate() is
    # called from jent_entropy_collector_alloc_internal(), so no configuration
    # avoids it.
    target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt)

    # The same dependency for pkg-config consumers. The exported CMake target
    # covers itself - CMake records a static library's PRIVATE dependencies as
    # $<LINK_ONLY:bcrypt> in its interface - but nothing propagates it into the
    # .pc file, so a static MinGW consumer building through pkg-config was left
    # with an undefined BCryptGenRandom. A shared build resolves the call inside
    # the DLL and needs nothing here.
    if(NOT BUILD_SHARED_LIBS)
        set(JITTER_PC_LIBS_PRIVATE "${JITTER_PC_LIBS_PRIVATE} -lbcrypt")
    endif()

    if(MSVC)
        # The Windows counterpart of the -z now,-z relro hardening applied on
        # the ELF targets above. /DYNAMICBASE used to sit in JITTER_C_FLAGS,
        # i.e. among the *compile* options: cl accepts that spelling without a
        # diagnostic but only forwards it when it drives the link itself, which
        # it never does under CMake, so the flag reached nothing. Both are
        # linker defaults today and are stated explicitly so that a toolchain
        # changing its defaults cannot silently drop ASLR or DEP.
        #
        # /GUARD:CF is deliberately not enabled: it also requires the matching
        # /guard:cf compile flag, which instruments indirect calls in the
        # entropy-collection core - code whose timing behaviour is the noise
        # source and is therefore not changed as a side effect of a build-system
        # cleanup.
        target_link_options(${PROJECT_NAME} PRIVATE /DYNAMICBASE /NXCOMPAT)
    endif()

    # JENT_PRIVATE_STATIC in the public header resolves to dllexport only while
    # the library itself is compiled; consumers get dllimport, or nothing at all
    # when they link the static archive. See jitterentropy.h.
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(${PROJECT_NAME} PRIVATE JENT_BUILDING_DLL)
    else()
        target_compile_definitions(${PROJECT_NAME} PUBLIC JENT_STATIC_LIB)
    endif()
endif()
# Only jitterentropy.h is installed (as PUBLIC_HEADER below). The arch/ headers
# are internal to the build - the public header defines struct jent_notime_ctx
# itself and includes none of them, and what they declare is not exported from
# the library - so they are deliberately not part of the installed tree.
install(TARGETS ${PROJECT_NAME}
    EXPORT jitterentropyTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT jitterentropyTargets
  FILE jitterentropyTargets.cmake
  NAMESPACE jitterentropy::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake
)

target_include_directories(${PROJECT_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/arch>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

if(EXTERNAL_CRYPTO STREQUAL "AWSLC")
    # This header is unique from OpenSSL
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

# Note: "NOT ANDROID", not "NOT ${ANDROID}" - with ANDROID undefined the
# expansion would leave "AND NOT" with no operand, which CMake evaluates to
# false, silently dropping the pthread link dependency on every regular build.
#
# "NOT WIN32" covers MSVC, clang-cl and MinGW alike: the internal timer uses
# the native Win32 threads there, which come from the platform and the CRT, so
# there is nothing to name on the link line. MSVC has no pthread.lib to link in
# the first place - naming it made every executable fail with LNK1104 - and
# MinGW would otherwise pull in winpthreads, which the Win32 back-end exists
# precisely to avoid depending on. Cygwin is a POSIX target and CMake does not
# set WIN32 for it, so it keeps pthreads.
if(INTERNAL_TIMER AND NOT ANDROID AND NOT WIN32)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_link_libraries(${PROJECT_NAME} PUBLIC pthread)
    endif()
endif()

# The same dependency, phrased for a target that cannot inherit it.
#
# The AMALGAMATED test program in tests/raw-entropy/recording_userspace #includes
# the library sources rather than linking the library, so nothing above reaches
# it and it has to resolve the internal timer's pthread_create() itself. That
# went unnoticed for a long time because the platforms built until now hide it:
# glibc 2.34 and later, Apple's libSystem and bionic all fold the pthread
# symbols into the C library. The BSDs keep them in a separate library, so
# OpenBSD was the first target where the link actually failed.
#
# Spelled -pthread rather than -lpthread because that is the form the BSDs need:
# on FreeBSD the library to link is libthr and only the driver flag selects it,
# which is exactly why FreeBSD is excluded from the -lpthread branch above.
# Every other non-Windows toolchain here accepts the flag. Windows needs no
# entry at all: _beginthreadex() and the Win32 thread calls resolve against the
# CRT and kernel32, both of which every Windows link already pulls in.
set(JITTER_THREAD_LIBRARIES "")
if(INTERNAL_TIMER AND NOT ANDROID AND NOT WIN32)
    set(JITTER_THREAD_LIBRARIES -pthread)
endif()

# testing/validation tools
add_subdirectory(tests/gcd)
add_subdirectory(tests/raw-entropy/validation-runtime)
add_subdirectory(tests/raw-entropy/recording_userspace)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Both talk to the kernel module's ioctl interface (linux_kernel/
    # jitterentropy_uapi.h pulls in <linux/ioctl.h>); Linux-only.
    add_subdirectory(tests/raw-entropy/recording_runtime_kernelspace)
    add_subdirectory(tests/chardev)
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
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/jitterentropyConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropyConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropyConfigVersion.cmake
  VERSION       ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropyConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/jitterentropyConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake
)

# man page(s)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/doc/jitterentropy.3
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man3)
