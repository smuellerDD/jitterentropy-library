# Runs one replay and checks both its report and its exit status. CTest ignores
# the status once PASS_REGULAR_EXPRESSION is set, so a crash or a sanitizer
# abort after the verdict was printed would otherwise pass. ASan and UBSan exit
# with 1 too, the status of a vector that fires, so their reports are matched
# as well.
#
# -DPROGRAM=<jitterentropy-health> -DVECTOR=<file> -DEXPECT_RC=<0|1|2>
# -DEXPECT_RE=<regex> [-DEMULATOR=<crosscompiling emulator>] [-DREQUIRES_LAG=1]
# [-DARGS=<arguments>]
# ARGS, a list, replaces "--replay <file>" - for the usage errors, which exit 2
# and say why on stderr, so EXPECT_RE is matched against both streams.
#
# A lag vector without the lag test compiled in cannot fire; CTest skips on the
# message below (SKIP_REGULAR_EXPRESSION).
if(REQUIRES_LAG)
    execute_process(COMMAND ${EMULATOR} ${PROGRAM} --lag-predictor
                    RESULT_VARIABLE _lag)
    # 1 is the answer; anything else (a crash, no binary) is a failure.
    if(_lag STREQUAL "1")
        message("skipped: the lag predictor is not built in")
        return()
    elseif(NOT _lag STREQUAL "0")
        message(FATAL_ERROR "--lag-predictor failed: ${_lag}")
    endif()
endif()

if(NOT DEFINED ARGS)
    set(ARGS --replay ${VECTOR})
endif()

execute_process(COMMAND ${EMULATOR} ${PROGRAM} ${ARGS}
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _out
                ERROR_VARIABLE _err)
message("${_out}${_err}")

if(NOT _rc STREQUAL EXPECT_RC)
    message(FATAL_ERROR "exit status ${_rc}, expected ${EXPECT_RC}")
endif()
if("${_out}${_err}" MATCHES "Sanitizer|runtime error:")
    message(FATAL_ERROR "a sanitizer reported an error")
endif()
if(NOT "${_out}${_err}" MATCHES "${EXPECT_RE}")
    message(FATAL_ERROR "the report does not match: ${EXPECT_RE}")
endif()
