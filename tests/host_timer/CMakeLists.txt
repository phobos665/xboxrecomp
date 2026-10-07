# host_timer -- the sub-millisecond timer in src/platform/host_timer.c.
#
# Checks that each wait waits at least as long as asked, not wildly longer,
# and that the event-or-time wait wakes on the event. The upper bounds are
# loose on purpose: CI runners are shared machines, and this is a test that
# the timer works, not a benchmark. The measured overshoot is printed.

add_executable(host_timer_test test_main.c)
target_link_libraries(host_timer_test PRIVATE platform)
add_test(NAME host_timer COMMAND host_timer_test)
