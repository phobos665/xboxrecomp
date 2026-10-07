# guest_memory -- the POSIX memory backend (src/platform/posix_memory.c) keeps
# the Windows placement, aliasing and 4 KB protection semantics the layout
# depends on, on a 16 KB-page host as well as a 4 KB one. POSIX only: on
# Windows the same calls are the real ones. Links only the platform library;
# no game files.

add_executable(guest_memory_test test_main.c)
target_link_libraries(guest_memory_test PRIVATE platform)

add_test(NAME guest_memory COMMAND guest_memory_test)
