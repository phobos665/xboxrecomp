# wma_decoder -- decode a WMA file to PCM through Media Foundation.
#
# From upstream PR #65, adapted to this tree's layout. Windows only: the
# decoder is Media Foundation, and the fixture is a 3.7 KB sine tone checked
# in with the test, so this needs no game files.
# test_main.c includes the decoder's source directly, so it is not listed
# here as well.
add_executable(wma_decoder_test test_main.c)
target_include_directories(wma_decoder_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio)
target_link_libraries(wma_decoder_test PRIVATE mfplat mfreadwrite mfuuid mf ole32)

add_test(NAME wma_decoder COMMAND wma_decoder_test
         "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine.wma")
