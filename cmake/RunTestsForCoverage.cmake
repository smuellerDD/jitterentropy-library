# Runs the whole CTest suite for the coverage report. Invoked in script mode
# from the "coverage" target with -DCTEST_EXECUTABLE=<ctest>.
#
# Unlike a gating run this includes the "unreliable" tests, which are what
# reaches the noise source and the health tests at runtime - without them the
# report describes little more than the startup path. Their result is not
# propagated: they can fail on properties of the machine, and whether they pass
# is the question `ctest -LE unreliable` answers, not this one.
execute_process(
    COMMAND ${CTEST_EXECUTABLE} --output-on-failure
    RESULT_VARIABLE _ctest_result)

if(_ctest_result)
    message(STATUS
        "Some tests failed (${_ctest_result}); the coverage report below "
        "covers what did run")
endif()
