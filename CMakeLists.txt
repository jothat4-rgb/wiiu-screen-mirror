cmake_minimum_required(VERSION 3.7)

project(ScreenMirror C CXX)

add_executable(ScreenMirror
    source/main.c
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)

target_include_directories(ScreenMirror PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_IMAGE_INCLUDE_DIRS}
)

target_link_directories(ScreenMirror PRIVATE
    ${SDL2_LIBRARY_DIRS}
    ${SDL2_IMAGE_LIBRARY_DIRS}
)

target_link_libraries(ScreenMirror
    ${SDL2_LIBRARIES}
    ${SDL2_IMAGE_LIBRARIES}
)

wut_create_rpx(ScreenMirror)
