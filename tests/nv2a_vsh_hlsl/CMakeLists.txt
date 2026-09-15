# nv2a_vsh_hlsl -- compile the HLSL the vertex-program translator generates.
#
# Windows only: it needs D3DCompile. Builds d3d8_vsh.c and nv2a_vsh.c with
# stubs for the three device accessors d3d8_vsh.c links against, the same way
# tests/d3d8_smoke builds its targets. No device, window or game files.

add_executable(nv2a_vsh_hlsl_test
    test_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_vsh.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/nv2a_vsh.c
)
target_include_directories(nv2a_vsh_hlsl_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
)
target_link_libraries(nv2a_vsh_hlsl_test PRIVATE
    platform d3d11 dxgi dxguid d3dcompiler
)

add_test(NAME nv2a_vsh_hlsl COMMAND nv2a_vsh_hlsl_test)
