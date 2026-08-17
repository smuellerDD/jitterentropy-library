set(CMAKE_C_STANDARD 11)

# Shared with validation-restart, hence the common parent directory.
add_executable(extractlsb ../extractlsb.c)
install(TARGETS extractlsb)
