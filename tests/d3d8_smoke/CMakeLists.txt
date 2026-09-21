# d3d8_smoke - offline smoke test for the pure xbox_d3d8 format layer.
# Compiles the real d3d8_resources.c + d3d8_swizzle.h with lightweight device
# accessor stubs so the format tables, predicates, swizzle classification and
# software conversions can be validated without a D3D11 device.

if(WIN32)
    add_executable(d3d8_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/test_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_resources.c
    )
    target_include_directories(d3d8_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
    )
    target_link_libraries(d3d8_smoke PRIVATE platform d3d11 dxgi dxguid d3dcompiler)
    if(MSVC)
        target_compile_options(d3d8_smoke PRIVATE /W3)
    endif()

    # Exercise bound state objects on D3D11's software device (no window/GPU).
    # d3d8_display.c comes along because the sampler path asks it whether
    # anisotropy is forced. Nothing here reaches its resolve pass, which is
    # the only part that wants a device.
    add_executable(d3d8_states_smoke
        test_states.c
        ../../src/d3d/d3d8_states.c
        ../../src/d3d/d3d8_display.c
    )
    target_include_directories(d3d8_states_smoke PRIVATE ../../src/d3d)
    target_link_libraries(d3d8_states_smoke PRIVATE platform d3d11 d3dcompiler)

    add_executable(d3d8_a8
        test_a8.c
        ../../src/d3d/d3d8_resources.c
        ../../src/d3d/d3d8_shaders.c
        ../../src/d3d/d3d8_combiners.c
    )
    target_include_directories(d3d8_a8 PRIVATE ../../src/d3d)
    target_link_libraries(d3d8_a8 PRIVATE platform d3d11 dxgi dxguid d3dcompiler)
endif()

if(WIN32)
    # Pure parsing of a pixel shader definition: no device, so it can run
    # under ctest. Links the same sources as d3d8_a8 because d3d8_combiners.c
    # reaches into the device layer for state it does not use here.
    add_executable(d3d8_final_default
        test_final_default.c
        ../../src/d3d/d3d8_resources.c
        ../../src/d3d/d3d8_shaders.c
        ../../src/d3d/d3d8_combiners.c
    )
    target_include_directories(d3d8_final_default PRIVATE ../../src/d3d)
    target_link_libraries(d3d8_final_default PRIVATE platform d3d11 dxgi dxguid d3dcompiler)
    # Registered with ctest from the root CMakeLists: this directory is
    # entered from src/d3d, before the root calls enable_testing().
endif()
