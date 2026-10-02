# The kernel's DES and modular exponentiation against published answers.

add_executable(crypto_soft
    test_main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel/xbox_crypto_soft.c)
target_include_directories(crypto_soft PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
if(MSVC)
    target_compile_options(crypto_soft PRIVATE /W4)
else()
    target_compile_options(crypto_soft PRIVATE -Wall -Wextra)
endif()
