# rngd-generate: jitterentropy-rngd --generate, its options and what --output
# refuses. It needs neither root nor the kernel.
#
#   cmake -DRNGD=<jitterentropy-rngd> -DDIR=<scratch directory> -P generate.cmake

# More than one read of the daemon (GENERATE_CHUNK), and not a multiple of it
set(_bytes 2500)

function(rngd_ok)
    execute_process(COMMAND "${RNGD}" ${ARGN} RESULT_VARIABLE _ret
                    OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _ret EQUAL 0)
        message(FATAL_ERROR "jitterentropy-rngd ${ARGN}: ${_ret}\n${_stderr}")
    endif()
    set(_stdout "${_stdout}" PARENT_SCOPE)
endfunction()

function(rngd_refused)
    execute_process(COMMAND "${RNGD}" ${ARGN} RESULT_VARIABLE _ret
                    OUTPUT_QUIET ERROR_QUIET)
    if(_ret EQUAL 0)
        message(FATAL_ERROR "jitterentropy-rngd ${ARGN} was not refused")
    endif()
endfunction()

function(check_hex _text _count)
    math(EXPR _want "2 * ${_count}")
    string(LENGTH "${_text}" _len)
    if(NOT _text MATCHES "^[0-9a-f]+$" OR NOT _len EQUAL _want)
        message(FATAL_ERROR "not ${_count} bytes as hex digits: \"${_text}\"")
    endif()
endfunction()

set(_raw "${DIR}/rngd-generate.bin")
set(_hex "${DIR}/rngd-generate.txt")
set(_link "${DIR}/rngd-generate.link")
set(_fifo "${DIR}/rngd-generate.fifo")
file(REMOVE "${_raw}" "${_hex}" "${_link}" "${_fifo}")

rngd_ok(--generate ${_bytes} --output "${_raw}")
file(SIZE "${_raw}" _size)
if(NOT _size EQUAL _bytes)
    message(FATAL_ERROR "${_raw}: ${_size} bytes, not ${_bytes}")
endif()

rngd_ok(--generate ${_bytes} --hex --output "${_hex}")
file(READ "${_hex}" _text)
check_hex("${_text}" ${_bytes})

# The log of -vv goes to stderr, stdout carries the bytes alone.
rngd_ok(--generate 32 --hex -vv)
check_hex("${_stdout}" 32)

# --status needs neither, and ends with 1 after the document.
execute_process(COMMAND "${RNGD}" --status RESULT_VARIABLE _ret
                OUTPUT_QUIET ERROR_VARIABLE _stderr)
if(NOT _ret EQUAL 1 OR NOT _stderr MATCHES "\"version\"")
    message(FATAL_ERROR "jitterentropy-rngd --status: ${_ret}\n${_stderr}")
endif()

rngd_refused(--generate 0)
rngd_refused(--generate -1)
rngd_refused(--hex)
rngd_refused(--output "${_hex}")
rngd_refused(--generate 8 --oneshot)
rngd_refused(--generate 8 --status)

# Not through a symbolic link, nor a hard link, and their target stays as it
# was
foreach(_kind SYMBOLIC HARD)
    file(REMOVE "${_link}")
    if(_kind STREQUAL "SYMBOLIC")
        file(CREATE_LINK "${_raw}" "${_link}" SYMBOLIC)
    else()
        file(CREATE_LINK "${_raw}" "${_link}")
    endif()
    rngd_refused(--generate 8 --output "${_link}")
    file(SIZE "${_raw}" _size)
    if(NOT _size EQUAL _bytes)
        message(FATAL_ERROR "${_raw} written through the ${_kind} link ${_link}")
    endif()
endforeach()
file(REMOVE "${_link}")

# Nor into a FIFO - without a reader the open must not block either
find_program(_mkfifo mkfifo)
if(_mkfifo)
    execute_process(COMMAND "${_mkfifo}" "${_fifo}" RESULT_VARIABLE _ret)
    if(NOT _ret EQUAL 0)
        message(FATAL_ERROR "mkfifo ${_fifo}: ${_ret}")
    endif()
    rngd_refused(--generate 8 --output "${_fifo}")
    file(REMOVE "${_fifo}")
endif()

# A regular file of the caller's own is overwritten
rngd_ok(--generate 16 --output "${_raw}")
file(SIZE "${_raw}" _size)
if(NOT _size EQUAL 16)
    message(FATAL_ERROR "${_raw}: ${_size} bytes after overwriting, not 16")
endif()
