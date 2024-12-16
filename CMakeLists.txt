cmake_minimum_required(VERSION 3.5)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(Queijo LANGUAGES CXX C)
add_subdirectory(luau)
add_subdirectory(libuv)

add_library(Queijo.Lib STATIC)
add_executable(Queijo.CLI)
set_target_properties(Queijo.CLI PROPERTIES OUTPUT_NAME queijo)

set(LIBUV_INCLUDE_DIR ${PROJECT_SOURCE_DIR/libuv/include})

message(libuv_include_dir="${LIBUV_INCLUDE_DIR}")

include(Sources.cmake)

target_compile_features(Queijo.Lib PUBLIC cxx_std_17)
target_include_directories(Queijo.Lib PUBLIC Lib/include ${LIBUV_INCLUDE_DIR})
target_link_libraries(Queijo.CLI PRIVATE Luau.Compiler Luau.Config Luau.CodeGen Luau.VM Queijo.Lib uv)

target_compile_options(Queijo.Lib PRIVATE ${QUEIJO_OPTIONS})
target_compile_options(Queijo.CLI PRIVATE ${QUEIJO_OPTIONS})

set(QUEIJO_OPTIONS)

if(MSVC)
    list(APPEND QUEIJO_OPTIONS /D_CRT_SECURE_NO_WARNINGS) # We need to use the portable CRT functions.
    list(APPEND QUEIJO_OPTIONS "/we4018") # Signed/unsigned mismatch
    list(APPEND QUEIJO_OPTIONS "/we4388") # Also signed/unsigned mismatch
    list(APPEND QUEIJO_OPTIONS /WX) # Warnings are errors
else()
    list(APPEND QUEIJO_OPTIONS -Wall) # All warnings
    list(APPEND QUEIJO_OPTIONS -Wimplicit-fallthrough)
    list(APPEND QUEIJO_OPTIONS -Wsign-compare) # This looks to be included in -Wall for GCC but not clang
    list(APPEND QUEIJO_OPTIONS -Werror) # Warnings are errors
endif()
