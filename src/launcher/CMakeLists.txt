# The launcher a recompiled title ships beside its game.
#
# Not a library and not built here: each title makes its own, because the
# executable it starts is the title's. A title's CMakeLists calls
#
#     recomp_add_launcher(timesplitters2_launcher timesplitters2_recomp.exe)
#
# and gets <name>.exe beside the game, reading the game's own XBE for the
# name it shows and the settings file it writes.
#
# Windows only for now. The drawing is behind launcher_theme.h so the day
# this needs to run elsewhere it is that file that is replaced, not the
# launcher.

if(WIN32)
    function(recomp_add_launcher TARGET GAME_EXE)
        add_executable(${TARGET} WIN32
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher.c
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher_theme.c
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher_bindings.c
        )
        target_include_directories(${TARGET} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
        )
        target_compile_definitions(${TARGET} PRIVATE
            LAUNCHER_GAME_EXE="${GAME_EXE}"
        )
        # xbox_config carries the settings file's format, so the launcher
        # and the runtime cannot disagree about what they are writing and
        # reading. xinput is the pad; the rest is in every Windows.
        target_link_libraries(${TARGET} PRIVATE
            xbox_config user32 gdi32 xinput9_1_0
        )
        if(MSVC)
            target_compile_options(${TARGET} PRIVATE
                /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
        endif()
    endfunction()
endif()
