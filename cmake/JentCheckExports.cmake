# Assert that a shared build exports the API of jitterentropy.h and nothing
# else. JENT_PRIVATE_STATIC on an internal declaration overrides
# -fvisibility=hidden, and the version script is not applied on macOS or
# Windows, so only the built library tells.
#
# nm prints one of three formats: BSD (the default of GNU, LLVM and Apple),
# POSIX (-P) and SVR4 (Solaris). Output none of them parses is a skip; a table
# that parses but holds no jent_ symbol fails.
#
# Every defined global symbol counts, not only the jent_ ones: on macOS nothing
# but the visibility limits the export set, and a global of another name leaks
# just the same. What the toolchain itself puts into every shared library is
# left out (see jent_toolchain_symbols below).
#
# Run in script mode with JENT_LIB, JENT_VERSION_SCRIPT, JENT_NM and JENT_NM_ARGS;
# JENT_REQUIRE_PARSE makes an unknown nm format an error instead of a skip.

foreach(var JENT_LIB JENT_VERSION_SCRIPT JENT_NM)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is not set")
    endif()
endforeach()

if(NOT EXISTS "${JENT_LIB}")
    message(FATAL_ERROR "no library at ${JENT_LIB}")
endif()

# Ask for the POSIX format; an nm rejecting -P is asked again without it.
execute_process(COMMAND "${JENT_NM}" -P ${JENT_NM_ARGS} "${JENT_LIB}"
                OUTPUT_VARIABLE nm_out
                ERROR_VARIABLE nm_err
                RESULT_VARIABLE nm_res)
if(NOT nm_res EQUAL 0)
    execute_process(COMMAND "${JENT_NM}" ${JENT_NM_ARGS} "${JENT_LIB}"
                    OUTPUT_VARIABLE nm_out
                    ERROR_VARIABLE nm_err
                    RESULT_VARIABLE nm_res)
endif()
if(NOT nm_res EQUAL 0)
    message(FATAL_ERROR "${JENT_NM} failed on ${JENT_LIB}: ${nm_res}\n${nm_err}")
endif()

# A symbol name.
set(sym "[A-Za-z_][A-Za-z0-9_.$@]*")

# Mach-O prefixes every C name with an underscore, which is dropped there -
# and only there, so that an ELF name that starts with one keeps it. Optional
# even there, for the few names the Mach-O toolchain spells without one.
if(nm_out MATCHES "(^|[ \t\n|])_jent_")
    set(us "_?")
else()
    set(us "")
endif()

# What the toolchain defines in every shared library, as the C names above
# leave them: the ELF section bounds and init/fini entries older binutils
# export and the Solaris link editor's PLT, the Mach-O image header, and what
# OpenBSD's crtbeginS.o links into every shared object (its atexit/atfork glue
# and the GCC Java hook).
set(jent_toolchain_symbols
    _init _fini __bss_start _edata _end _etext __end__ __bss_end__
    __bss_start__ _bss_end__ __data_start _DYNAMIC _GLOBAL_OFFSET_TABLE_
    _PROCEDURE_LINKAGE_TABLE_
    _mh_dylib_header _mh_execute_header dyld_stub_binder
    _Jv_RegisterClasses __cxa_atexit __cxa_finalize _thread_atfork)

# nm's type letters: upper case for a global, lower case for a local - except
# the undefined weak symbols w and v, and GNU's i (indirect function) and u
# (unique global), which are global although lower case.
function(jent_nm_type_exported type out)
    if(type MATCHES "^[A-TV-Ziu]$")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

string(REPLACE "\n" ";" nm_lines "${nm_out}")
set(exported "")
set(parsed 0)
foreach(line IN LISTS nm_lines)
    set(name "")
    set(defined TRUE)

    if(line MATCHES "^${us}(${sym})[ \t]+([A-Za-z])([ \t]|$)")
        # POSIX: "<name> <type> <value> <size>".
        set(name "${CMAKE_MATCH_1}")
        jent_nm_type_exported("${CMAKE_MATCH_2}" defined)
    elseif(line MATCHES "^[0-9a-fA-F]*[ \t]+([A-Za-z])[ \t]+${us}(${sym})")
        # BSD: "<value> <type> <name>", an undefined symbol carrying no value.
        set(name "${CMAKE_MATCH_2}")
        jent_nm_type_exported("${CMAKE_MATCH_1}" defined)
    elseif(line MATCHES "^\\[[0-9]+\\][ \t]*\\|.*\\|[ \t]*${us}(${sym})[ \t]*$")
        # SVR4: "[i] |value|size|type|bind|other|shndx|name", the section
        # index of an undefined symbol being UNDEF. Only GLOB and WEAK are
        # exported: nm -g leaves the LOCL rows out already, but the check
        # does not rely on the arguments it is given.
        set(name "${CMAKE_MATCH_1}")
        if(line MATCHES "\\|[ \t]*UNDEF" OR
           NOT line MATCHES "\\|[ \t]*(GLOB|WEAK)[ \t]*\\|")
            set(defined FALSE)
        endif()
    else()
        continue()
    endif()

    math(EXPR parsed "${parsed} + 1")
    if(defined)
        list(FIND jent_toolchain_symbols "${name}" toolchain)
        if(toolchain EQUAL -1)
            list(APPEND exported "${name}")
        endif()
    endif()
endforeach()

if(parsed EQUAL 0)
    # An unknown format rather than an empty export set: skip, never pass,
    # and quote the output so the format can be added.
    string(REPLACE ";" "\n" nm_head "${nm_lines}")
    string(LENGTH "${nm_head}" nm_len)
    if(nm_len GREATER 400)
        string(SUBSTRING "${nm_head}" 0 400 nm_head)
    endif()
    # A skip is CTest's reading of the message below; a caller without
    # CTest (the Makefile CI job) sets JENT_REQUIRE_PARSE, as the exit status
    # is all it sees.
    if(JENT_REQUIRE_PARSE)
        message(FATAL_ERROR
            "the export check does not know the output format of ${JENT_NM}; "
            "no symbol parsed from:\n${nm_head}")
    endif()
    message("the export check does not know the output format of ${JENT_NM}")
    message("no symbol parsed from:\n${nm_head}")
    return()
endif()

if(NOT exported MATCHES "(^|;)jent_")
    message(FATAL_ERROR
        "${JENT_NM} parsed ${parsed} symbols in ${JENT_LIB} but not one "
        "jent_* among them - the shared library exports none of its API")
endif()

# The allowed set: the global block of the version script.
file(STRINGS "${JENT_VERSION_SCRIPT}" script_lines)
set(allowed "")
set(in_global FALSE)
foreach(line IN LISTS script_lines)
    if(line MATCHES "global:")
        set(in_global TRUE)
    elseif(line MATCHES "local:")
        set(in_global FALSE)
    elseif(in_global AND line MATCHES "(jent_[A-Za-z0-9_]+)")
        list(APPEND allowed "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(REMOVE_DUPLICATES exported)
list(REMOVE_DUPLICATES allowed)
list(SORT exported)
list(SORT allowed)

if(exported STREQUAL allowed)
    list(LENGTH exported n)
    message(STATUS "the shared library exports the ${n} functions of the API "
                   "and nothing else")
    return()
endif()

set(extra ${exported})
set(missing ${allowed})
if(allowed)
    list(REMOVE_ITEM extra ${allowed})
endif()
if(exported)
    list(REMOVE_ITEM missing ${exported})
endif()

message(FATAL_ERROR
    "the shared library does not export the API of version.lds. "
    "Exported but internal: ${extra}. Declared but not exported: ${missing}. "
    "An internal function appearing here is usually JENT_PRIVATE_STATIC on "
    "its declaration, which is the marker of the API and overrides "
    "-fvisibility=hidden.")
