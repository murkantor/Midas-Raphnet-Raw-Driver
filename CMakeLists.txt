cmake_minimum_required(VERSION 3.13)
project(raphnet_raw_driver C)

# midas_feel_raphnet : raphnet N64 RAW adapter driver.
#   Windows : native Win32 HID (link hid, setupapi) - no SDL.
#   others  : SDL2 >= 2.0.18 SDL_hid_* (RAPHNET_RAW_SDL3=ON for SDL3).
#
# As a subproject:   add_subdirectory(third_party/raphnet-raw-driver)
#                    target_link_libraries(game PRIVATE midas_feel_raphnet)
# An existing SDL2::SDL2 / SDL3::SDL3 target in the parent is reused.

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(RAPHNET_RAW_TOP ON)
else()
    set(RAPHNET_RAW_TOP OFF)
endif()

option(RAPHNET_RAW_SDL3 "Use SDL3 instead of SDL2 for the non-Windows transport" OFF)
option(RAPHNET_RAW_TOOLS "Build mf_raphnet_probe" ${RAPHNET_RAW_TOP})

add_library(midas_feel_raphnet STATIC src/mf_raphnet.c)
target_include_directories(midas_feel_raphnet PUBLIC include)
set_property(TARGET midas_feel_raphnet PROPERTY C_STANDARD 99)

if(WIN32)
    target_link_libraries(midas_feel_raphnet PUBLIC hid setupapi)
elseif(RAPHNET_RAW_SDL3)
    if(NOT TARGET SDL3::SDL3)
        find_package(SDL3 REQUIRED)
    endif()
    target_compile_definitions(midas_feel_raphnet PUBLIC MF_RAPHNET_SDL3)
    target_link_libraries(midas_feel_raphnet PUBLIC SDL3::SDL3)
else()
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 REQUIRED)
    endif()
    target_link_libraries(midas_feel_raphnet PUBLIC SDL2::SDL2)
endif()

if(RAPHNET_RAW_TOOLS)
    add_executable(mf_raphnet_probe tools/mf_raphnet_probe.c)
    target_link_libraries(mf_raphnet_probe PRIVATE midas_feel_raphnet)
endif()
