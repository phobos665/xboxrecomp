# memory_layout -- the real xbox_MemoryLayoutInit on a synthetic XBE, for a
# 64 MB map and for a 128 MB one (40 MB of demand-loaded sections, BLiNX's
# shape), with every mirror, the contiguous/tiled alias, the device apertures,
# the 4 KB trap pages, the kernel PE header and the TIB checked. One map per
# process, so two tests. POSIX (setenv/_exit); no game files.

add_executable(memory_layout_test test_main.c)
target_link_libraries(memory_layout_test PRIVATE xboxrecomp)

add_test(NAME memory_layout_64  COMMAND memory_layout_test 64)
add_test(NAME memory_layout_128 COMMAND memory_layout_test 128)
