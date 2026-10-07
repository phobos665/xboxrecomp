# compat_threads -- the POSIX Win32 thread shims in src/platform/win32_compat.c:
# fibers, GetCurrentThreadStackLimits, SuspendThread/ResumeThread.
# POSIX only; on Windows these are kernel32's own.

add_executable(compat_threads_test test_main.c)
target_link_libraries(compat_threads_test PRIVATE platform)
add_test(NAME compat_threads COMMAND compat_threads_test)
