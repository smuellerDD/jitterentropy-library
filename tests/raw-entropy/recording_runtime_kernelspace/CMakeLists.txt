set(CMAKE_C_STANDARD 11)

add_executable(getrawentropy getrawentropy.c)
# Only the JENT_* flag constants are needed from the top-level header and the
# ioctl ABI from the kernel module's UAPI header; the tool does not link
# against the library.
target_include_directories(getrawentropy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../linux_kernel)
# The out-of-tree module in linux_kernel/ delivers u64 raw samples, as does
# the in-kernel Jitter RNG since Linux 6.13. When targeting the in-kernel
# test interface of older kernels, compile manually without this define (see
# the comment at the top of getrawentropy.c).
target_compile_definitions(getrawentropy PRIVATE RAW_DATATYPE_U64)
install(TARGETS getrawentropy)
