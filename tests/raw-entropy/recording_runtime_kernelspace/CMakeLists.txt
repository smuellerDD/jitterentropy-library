set(CMAKE_C_STANDARD 11)

add_executable(getrawentropy getrawentropy.c)
# Only the JENT_* constants and the module's ioctl ABI; no library link.
target_include_directories(getrawentropy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../linux_kernel)
# u64 raw samples, as the in-kernel Jitter RNG also delivers since 6.13. For
# older kernels compile manually without this (see getrawentropy.c).
target_compile_definitions(getrawentropy PRIVATE RAW_DATATYPE_U64)
install(TARGETS getrawentropy)
