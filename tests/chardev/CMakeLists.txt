set(CMAKE_C_STANDARD 11)

add_executable(jitterentropy-chardev-status jitterentropy-chardev-status.c)
# The ioctl ABI comes from the module's UAPI header; no library link.
target_include_directories(jitterentropy-chardev-status PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../linux_kernel)
install(TARGETS jitterentropy-chardev-status)

add_executable(jitterentropy-chardev-fields jitterentropy-chardev-fields.c)
# Also the top-level header, for the JENT_* flag and health failure bits the
# status document is checked against; still no library link.
target_include_directories(jitterentropy-chardev-fields PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../linux_kernel)
install(TARGETS jitterentropy-chardev-fields)
