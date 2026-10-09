# kernel_directory_posix -- closing a directory handle releases its search on
# the POSIX file backend, through NtClose and through the bridge's close path.
# Includes src/kernel/kernel_file.c to inspect its private table. No game files.

add_executable(kernel_directory_posix_test test_main.c)
target_include_directories(kernel_directory_posix_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel)
target_link_libraries(kernel_directory_posix_test PRIVATE xboxrecomp)

add_test(NAME kernel_directory_posix COMMAND kernel_directory_posix_test)
