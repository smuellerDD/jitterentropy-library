set(CMAKE_C_STANDARD 11)

# Shared with validation-restart, hence the common parent directory.
add_executable(extractlsb ../extractlsb.c)
# The same flags every other program of this tree is built with. Without them
# this was the one target the project's own warning settings never saw.
target_compile_options(extractlsb PRIVATE ${JITTER_C_FLAGS})
install(TARGETS extractlsb)
