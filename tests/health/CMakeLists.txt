# Compiles src/jitterentropy-health.c into itself to reach the static per-test
# insert functions, and so must NOT also link the library: the duplicates
# collide on Windows and are masked only by -fvisibility=hidden on ELF.
add_executable(jitterentropy-health health.c)

# Against the public header as the library builds it, as the AMALGAMATED
# programs do.
target_compile_definitions(jitterentropy-health PRIVATE JENT_STATIC_LIB)
target_compile_options(jitterentropy-health PRIVATE ${JITTER_C_FLAGS})
target_include_directories(jitterentropy-health PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    # arch/, for the atomic accessors the health tests reach through
    # jent_health_failure(); absorbed here like the health source itself.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../arch)

if(ENABLE_TOOLS)
    install(TARGETS jitterentropy-health)
endif()

if(BUILD_TESTING)

# One per OSR, not one covering all, because every cutoff derives from it.
#
# What this catches is the BEHAVIOUR at a cutoff: each case feeds one sample
# short of it and then the decisive one, so an off-by-one, an inverted
# comparison or a permanent cutoff that fires where the intermittent one should
# fails here at the oversampling rate where it is wrong. What it cannot catch
# is a wrong VALUE in a cutoff table: the cases read the cutoff out of the
# collector the implementation set up, so a table fed to itself agrees with
# itself. The health-cutoff-tables case below is what judges the values.
#
# The bounds are read from the header that defines them, the way the top level
# CMakeLists.txt reads the version out of jitterentropy.h. A change of
# JENT_MAX_OSR there (with the cutoff tables extended to it) would otherwise
# leave the cutoffs of every OSR above 20 untested while still reporting
# success. The Makefile in this directory derives the same sweep.
foreach(_jent_osr_bound JENT_MIN_OSR JENT_MAX_OSR)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../jitterentropy.h"
         _jent_osr_line
         REGEX "^#define[ \t]+${_jent_osr_bound}[ \t]+[0-9]+")
    string(REGEX MATCH "[0-9]+" ${_jent_osr_bound} "${_jent_osr_line}")
    if(NOT ${_jent_osr_bound} MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "Cannot parse ${_jent_osr_bound} from jitterentropy.h")
    endif()
endforeach()

foreach(_osr RANGE ${JENT_MIN_OSR} ${JENT_MAX_OSR})
    add_test(NAME health-induced-failure-osr-${_osr}
             COMMAND jitterentropy-health ${_osr})
endforeach()

# And the values themselves: cutoffs.py recomputes all 15 tables and both RCT
# macros from the SP800-90B formulas with mpmath - an independent derivation,
# not a second copy of the constants - and --check compares them against the
# source. It needs python3 with mpmath, which a build host need not have, so
# without it the case reports a skip rather than passing silently or failing.
# CI sets JENT_REQUIRE_CUTOFF_CHECK so that a lost mpmath fails the configure
# instead of turning the only check of the values into a skip.
option(JENT_REQUIRE_CUTOFF_CHECK
       "Fail the configure if health-cutoff-tables would be skipped" OFF)
find_program(JITTER_PYTHON3 NAMES python3 python)
set(_cutoffs_skip "")
if(NOT JITTER_PYTHON3)
    set(_cutoffs_skip "python3 was not found")
else()
    execute_process(COMMAND ${JITTER_PYTHON3} -c "import mpmath"
                    RESULT_VARIABLE _mpmath
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT _mpmath EQUAL 0)
        set(_cutoffs_skip "${JITTER_PYTHON3} has no mpmath module")
    endif()
endif()

if(_cutoffs_skip AND JENT_REQUIRE_CUTOFF_CHECK)
    message(FATAL_ERROR "JENT_REQUIRE_CUTOFF_CHECK: ${_cutoffs_skip}")
elseif(_cutoffs_skip)
    add_test(NAME health-cutoff-tables
             COMMAND ${CMAKE_COMMAND} -E echo
                     "skipped: the cutoff derivation needs mpmath (${_cutoffs_skip})")
else()
    add_test(NAME health-cutoff-tables
             COMMAND ${JITTER_PYTHON3}
                     ${CMAKE_CURRENT_SOURCE_DIR}/cutoffs.py --check
                     ${CMAKE_CURRENT_SOURCE_DIR}/../../src/jitterentropy-health.c)
endif()
set_tests_properties(health-cutoff-tables PROPERTIES
                     SKIP_REGULAR_EXPRESSION "skipped: the cutoff derivation")

# Replay mode against testdata/: one vector per failure a file of time stamps
# can produce, plus one that must produce none - without which a replay failing
# everything would look correct. Matched on the output, the exit status being 1
# whenever anything fires and unable to tell the vectors apart; checked as
# well, by replay.cmake.
#
# The memory RCT is reported once it reaches its cutoff; its recovery loop needs
# fresh data and is not replayed.
#
# The isolating vectors match the whole report, so they assert nothing else
# fired either.
set(JITTER_HEALTH_VECTORS
    rct-intermittent
        "fired:\n +repetition count test \\(RCT\\) +intermittent\n +note:"
    rct-permanent
        "fired:\n +repetition count test \\(RCT\\) +intermittent and permanent\n +note:"
    lag-intermittent
        "fired:\n +lag predictor test +intermittent\n +note:"
    lag-permanent
        "fired:\n +lag predictor test +intermittent and permanent\n +note:"
    apt-intermittent
        "fired:\n +adaptive proportion test \\(APT\\) +intermittent\n +note:"
    apt-permanent
        "fired:\n +adaptive proportion test \\(APT\\) +intermittent and permanent\n +note:"
    rct-mem
        "fired:\n +repetition count test with memory +cutoff reached 1 time\\(s\\), recovery not replayed\n +note:"
    no-failure
        "no health test fired"
)

list(LENGTH JITTER_HEALTH_VECTORS _n)
math(EXPR _last "${_n} / 2 - 1")
foreach(_i RANGE ${_last})
    math(EXPR _name_idx "${_i} * 2")
    math(EXPR _re_idx "${_i} * 2 + 1")
    list(GET JITTER_HEALTH_VECTORS ${_name_idx} _name)
    list(GET JITTER_HEALTH_VECTORS ${_re_idx} _re)

    if(_name MATCHES "^lag-")
        set(_lag 1)
    else()
        set(_lag 0)
    endif()

    if(_name STREQUAL "no-failure")
        set(_rc 0)
    else()
        set(_rc 1)
    endif()

    add_test(NAME health-replay-${_name}
             COMMAND ${CMAKE_COMMAND}
                     -DPROGRAM=$<TARGET_FILE:jitterentropy-health>
                     -DVECTOR=${CMAKE_CURRENT_SOURCE_DIR}/testdata/${_name}.txt
                     -DEXPECT_RC=${_rc}
                     "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                     -DREQUIRES_LAG=${_lag}
                     "-DEXPECT_RE=${_re}"
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/replay.cmake)
    set_tests_properties(health-replay-${_name} PROPERTIES
                         SKIP_REGULAR_EXPRESSION "lag predictor is not built in")
endforeach()

# A usage error exits 2 - no verdict, like an unreadable file - and not 1, the
# status of a health test that fired, which a driver would take as a finding
# about the recording. osr 0 is below JENT_MIN_OSR.
add_test(NAME health-usage-replay-no-file
         COMMAND ${CMAKE_COMMAND}
                 -DPROGRAM=$<TARGET_FILE:jitterentropy-health>
                 -DARGS=--replay
                 -DEXPECT_RC=2
                 "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                 "-DEXPECT_RE=--replay needs a file"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/replay.cmake)
add_test(NAME health-usage-bad-osr
         COMMAND ${CMAKE_COMMAND}
                 -DPROGRAM=$<TARGET_FILE:jitterentropy-health>
                 "-DARGS=--replay;${CMAKE_CURRENT_SOURCE_DIR}/testdata/no-failure.txt;0"
                 -DEXPECT_RC=2
                 "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                 "-DEXPECT_RE=Oversampling rate must be in the range"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/replay.cmake)

# A sign behind the 0x is no time stamp either: strtoull() took "0x-5" as
# -5 and the replay judged a stamp near 2^64. Written at configure time rather
# than shipped in testdata/, which holds recordings.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hex-sign.txt "0x10\n0x-5\n")
add_test(NAME health-usage-hex-sign
         COMMAND ${CMAKE_COMMAND}
                 -DPROGRAM=$<TARGET_FILE:jitterentropy-health>
                 -DVECTOR=${CMAKE_CURRENT_BINARY_DIR}/hex-sign.txt
                 -DEXPECT_RC=2
                 "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                 "-DEXPECT_RE=:2: not a time stamp: 0x-5"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/replay.cmake)

endif()

# Shipped with the tool: a validation has to show which data gave which
# verdict. The replay tests above read them from the source tree instead, so
# this is needed for the install and not for them.
if(ENABLE_TOOLS)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/testdata
            DESTINATION ${CMAKE_INSTALL_DATADIR}/jitterentropy)
endif()
