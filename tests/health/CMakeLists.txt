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
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

if(ENABLE_TOOLS)
    install(TARGETS jitterentropy-health)
endif()

if(BUILD_TESTING)

# One per OSR, not one covering all: every cutoff derives from it, and a table
# entry wrong for a single OSR is what this catches.
foreach(_osr RANGE 3 20)
    add_test(NAME health-induced-failure-osr-${_osr}
             COMMAND jitterentropy-health ${_osr})
endforeach()

# Replay mode against testdata/: one vector per failure a file of time stamps
# can produce, plus one that must produce none - without which a replay failing
# everything would look correct. Matched on the output, the exit status being 1
# whenever anything fires and unable to tell the vectors apart.
#
# The memory RCT is absent on purpose: at its cutoff it enters a recovery loop
# generating fresh data, which a replay has none to do. The induced failure mode
# reaches that state directly.
#
# The four isolating vectors match the whole report, so they assert nothing else
# fired either. The APT pair cannot: no time stamps reach its cutoff without
# passing the RCT and lag ones on the way.
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
        "adaptive proportion test \\(APT\\) +intermittent\n"
    apt-permanent
        "adaptive proportion test \\(APT\\) +intermittent and permanent"
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

    add_test(NAME health-replay-${_name}
             COMMAND jitterentropy-health --replay
                     ${CMAKE_CURRENT_SOURCE_DIR}/testdata/${_name}.txt)
    set_tests_properties(health-replay-${_name}
                         PROPERTIES PASS_REGULAR_EXPRESSION "${_re}")
endforeach()

endif()

# Shipped with the tool: a validation has to show which data gave which
# verdict. The replay tests above read them from the source tree instead, so
# this is needed for the install and not for them.
if(ENABLE_TOOLS)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/testdata
            DESTINATION ${CMAKE_INSTALL_DATADIR}/jitterentropy)
endif()
