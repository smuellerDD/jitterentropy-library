set(CMAKE_C_STANDARD 11)

add_executable(jitterentropy-chardev-status jitterentropy-chardev-status.c)
# The ioctl ABI comes from the kernel module's UAPI header; the tool does not
# link against the library.
target_include_directories(jitterentropy-chardev-status PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../linux_kernel)
install(TARGETS jitterentropy-chardev-status)
