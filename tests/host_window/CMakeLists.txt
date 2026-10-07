# host_window -- the background window promise (src/host/host.h).
#
# Opens the game window the way a title would, with RECOMP_WINDOW_BACKGROUND
# forced on, from a thread that is not the main one, and fails if the window
# ever has the keyboard focus or the process ever becomes the active
# application. Needs a display; not a ctest (a CI runner may have none), run
# it by hand:  host_window_focus [seconds]
# check_focus.sh beside it also asks the window server which application is in
# front before, during and after.

add_executable(host_window_focus host_window_focus.c)
target_link_libraries(host_window_focus PRIVATE xbox_host)
