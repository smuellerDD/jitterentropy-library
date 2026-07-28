cmake_minimum_required(VERSION 3.14)

project(jitterentropy C)
set(CMAKE_C_STANDARD 11)

# jitterentropy-osr.c calls floor() and ceil(). Whether those need an explicit
# -lm depends on the C library: glibc before 2.34 and musl keep them in libm,
# newer glibc, macOS and mingw-w64 resolve them from the default libraries.
#
# This is deliberately not find_library(MATH_LIBRARY m). That searches library
# *paths* and never sees CMAKE_C_FLAGS, so under a flag-selected target - most
# obviously -m32 - it fails to turn up a usable libm, reports NOTFOUND, and the
# guard below then silently drops -lm from the link. The result was a link that
# died on "undefined reference to floor". check_symbol_exists() compiles and
# links a probe with the real compiler and the real flags, so it answers for the
# target actually being built.
include(CheckSymbolExists)
check_symbol_exists(floor "math.h" JENT_FLOOR_IN_DEFAULT_LIBS)
if(NOT JENT_FLOOR_IN_DEFAULT_LIBS)
    set(CMAKE_REQUIRED_LIBRARIES m)
    check_symbol_exists(floor "math.h" JENT_FLOOR_IN_LIBM)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(JENT_FLOOR_IN_LIBM)
        set(MATH_LIBRARY m)
    else()
        message(FATAL_ERROR
            "floor() is available neither by default nor with -lm")
    endif()
endif()

# Two kinds of program are built here.
#
# Most are ordinary consumers of the library and link it like any other user
# would. The exception is marked AMALGAMATED: that program #includes every
# library .c file directly (see the include block at the top of the source) so
# that it can instrument internals the installed library does not export. It is
# therefore already a complete copy of the library and must NOT also link it -
# on Windows the defining and the importing copy collide outright ("multiple
# definition of jent_read_entropy"), while on ELF the duplicate was merely
# masked by -fvisibility=hidden letting the local copy win. It also has to
# satisfy the dependencies of the sources it absorbed, and to build against the
# public header as the library does rather than as a consumer.
function(testprogram name)
    cmake_parse_arguments(TP "AMALGAMATED" "" "" ${ARGN})
    add_executable(${name} ${name}.c)

    if(TP_AMALGAMATED)
        target_compile_definitions(${name} PRIVATE JENT_STATIC_LIB)
        # arch/ is normally supplied by the library target's interface, which
        # this program deliberately does not use.
        target_include_directories(${name} PRIVATE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../arch>)
        if(WIN32)
            # Carries its own copy of arch/jitterentropy-arch-uuid.c and so its
            # own BCryptGenRandom() call to resolve.
            target_link_libraries(${name} "bcrypt")
        endif()
        # Likewise its own copy of the internal timer, so its own
        # pthread_create(). The library target carries this dependency in its
        # link interface, but that interface is reached by linking the library,
        # which this program deliberately does not do. See the definition of
        # JITTER_THREAD_LIBRARIES in the top-level CMakeLists.txt.
        target_link_libraries(${name} ${JITTER_THREAD_LIBRARIES})
        # And for the same reason its own external crypto dependency: with
        # EXTERNAL_CRYPTO set, the sources absorbed here compile against
        # <openssl/evp.h> or <gcrypt.h> and call into that library, while the
        # header and library locations the top-level CMakeLists.txt found are
        # attached to the library target this program does not link. Without
        # these two lines an EXTERNAL_CRYPTO build fails on the include and,
        # once that is satisfied, on OPENSSL_secure_malloc() at link time.
        # Empty and harmless in the default build, where EXTERNAL_CRYPTO is off.
        if(EXTERNAL_CRYPTO)
            target_include_directories(${name} PRIVATE ${LIBCRYPTO_INCLUDE_DIR})
            target_link_libraries(${name} ${LIBCRYPTO_LIBRARY})
        endif()
        # The stack protector needs no equivalent line: the top-level
        # CMakeLists.txt puts its flag on every target's link command with
        # add_link_options(), precisely because relying on the library's link
        # interface for it does not survive a shared build.
    else()
        target_link_libraries(${name} ${PROJECT_NAME})
    endif()

    if(MATH_LIBRARY)
        target_link_libraries(${name} ${MATH_LIBRARY})
    endif()
    # No target_link_directories() here: the library is linked through the
    # ${PROJECT_NAME} target above, which carries its own location. The
    # hard-coded ../../../build path this used to add does not exist for any
    # out-of-tree build directory, and Apple's ld64 warns about every missing
    # search path ("ld: warning: search path ... not found").
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../src>)
    target_compile_options(${name} PRIVATE ${JITTER_C_FLAGS})
    include(GNUInstallDirs)
    install(TARGETS ${name})
endfunction()

testprogram(jitterentropy-hashtime AMALGAMATED)
testprogram(jitterentropy-osr)
testprogram(jitterentropy-rng)
