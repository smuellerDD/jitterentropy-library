# Only ever add_subdirectory()'d from the top-level CMakeLists.txt, whose
# library target and JITTER_C_FLAGS everything below uses. No project() here: a
# second project(jitterentropy) rewrote the cached jitterentropy_SOURCE_DIR and
# jitterentropy_BINARY_DIR to this directory for everything configured after
# it, and cleared PROJECT_VERSION here.
set(CMAKE_C_STANDARD 11)

# Whether floor() and ceil() need -lm depends on the C library. Deliberately
# not find_library(MATH_LIBRARY m), which searches library paths and never sees
# CMAKE_C_FLAGS: under -m32 it reports NOTFOUND and the guard below then drops
# -lm. check_symbol_exists() probes with the real compiler and flags.
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

# Most of these link the library. The AMALGAMATED one #includes every library
# .c file instead, to instrument internals the library does not export, and so
# must NOT also link it: the copies collide on Windows and are masked only by
# -fvisibility=hidden on ELF. It also carries the absorbed sources'
# dependencies and builds against the public header as the library does.
function(testprogram name)
    cmake_parse_arguments(TP "AMALGAMATED" "" "" ${ARGN})
    add_executable(${name} ${name}.c)

    if(TP_AMALGAMATED)
        target_compile_definitions(${name} PRIVATE JENT_STATIC_LIB)
        # Normally supplied by the library target's interface.
        target_include_directories(${name} PRIVATE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../arch>)
        if(WIN32)
            # Its own copy of the UUID backend, so its own BCryptGenRandom().
            target_link_libraries(${name} "bcrypt")
        endif()
        # Likewise its own timer, so its own pthread_create(). See
        # JITTER_SYSTEM_LIBRARIES in the top-level CMakeLists.txt.
        target_link_libraries(${name} ${JITTER_SYSTEM_LIBRARIES})
        # The stack protector needs none: add_link_options() covers it.
    else()
        target_link_libraries(${name} ${PROJECT_NAME})
    endif()
    # jitterentropy-memlock.h sets up the external crypto's secure heap
    # itself, and the library links that dependency privately. Empty in the
    # default build.
    if(EXTERNAL_CRYPTO)
        target_include_directories(${name} PRIVATE ${LIBCRYPTO_INCLUDE_DIR})
        target_link_libraries(${name} ${LIBCRYPTO_LIBRARY})
    endif()

    if(MATH_LIBRARY)
        target_link_libraries(${name} ${MATH_LIBRARY})
    endif()
    # No target_link_directories(): the target carries its own location, and
    # the hard-coded path this used to add exists for no out-of-tree build.
    # ../.. is tests/, for jitterentropy-memlock.h - shared with the unit tests.
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>)
    target_compile_options(${name} PRIVATE ${JITTER_C_FLAGS})
    include(GNUInstallDirs)
    if(ENABLE_TOOLS)
        install(TARGETS ${name})
    endif()
endfunction()

# jitterentropy-rng is also what the rng-* tests run, so it is built whenever
# either option is on; the other two are tools alone.
testprogram(jitterentropy-rng)

# The flags word encoded and decoded from the header's JENT_* macros alone:
# nothing linked, and deterministic, so its tests gate.
add_executable(jitterentropy-flags jitterentropy-flags.c)
target_include_directories(jitterentropy-flags PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
target_compile_options(jitterentropy-flags PRIVATE ${JITTER_C_FLAGS})
if(ENABLE_TOOLS)
    install(TARGETS jitterentropy-flags)
endif()
if(BUILD_TESTING)
    # JENT_NTG1 is 64, JENT_MAX_MEMSIZE_1MB 11 << 27, JENT_HASHLOOP_16 5 << 23
    add_test(NAME flags-encode
             COMMAND jitterentropy-flags ntg1 JENT_MAX_MEMSIZE_1MB
                     --hloopcnt 4)
    set_tests_properties(flags-encode PROPERTIES
        PASS_REGULAR_EXPRESSION "^1518338112\n$")
    add_test(NAME flags-decode
             COMMAND jitterentropy-flags --decode 0x5a800040)
    set_tests_properties(flags-decode PROPERTIES
        PASS_REGULAR_EXPRESSION "JENT_NTG1 +0x00000040  set.*JENT_HASHLOOP_16.*JENT_MAX_MEMSIZE_1MB")
    # The prefix is case-insensitive like the rest of the name
    add_test(NAME flags-encode-mixed-case
             COMMAND jitterentropy-flags JeNt_NtG1)
    set_tests_properties(flags-encode-mixed-case PROPERTIES
        PASS_REGULAR_EXPRESSION "^64\n$")
    add_test(NAME flags-decode-not-first
             COMMAND jitterentropy-flags ntg1 --decode 64)
    set_tests_properties(flags-decode-not-first PROPERTIES WILL_FAIL TRUE)
    # What the library refuses fails
    add_test(NAME flags-decode-reserved
             COMMAND jitterentropy-flags --decode 0x200)
    add_test(NAME flags-encode-conflict
             COMMAND jitterentropy-flags --ntg1 --force-internal-timer)
    add_test(NAME flags-encode-field-twice
             COMMAND jitterentropy-flags JENT_HASHLOOP_1 JENT_HASHLOOP_2)
    set_tests_properties(flags-decode-reserved flags-encode-conflict
        flags-encode-field-twice PROPERTIES WILL_FAIL TRUE)
endif()
if(ENABLE_TOOLS)
    testprogram(jitterentropy-hashtime AMALGAMATED)
    testprogram(jitterentropy-osr)

    # The CPU information tool only queries the operating system about the
    # CPUs - it does not link against the library.
    add_executable(jitterentropy-cpuinfo jitterentropy-cpuinfo.c)
    target_compile_options(jitterentropy-cpuinfo PRIVATE ${JITTER_C_FLAGS})
    if(WIN32)
        # The model of each CPU is read from the registry with RegGetValueA(),
        # which MSVC pulls in through its default libraries and MinGW does not.
        target_link_libraries(jitterentropy-cpuinfo "advapi32")
    endif()
    install(TARGETS jitterentropy-cpuinfo)
endif()
