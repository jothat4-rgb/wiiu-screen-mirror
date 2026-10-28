
cmake_minimum_required(VERSION 3.7)

project(ScreenMirror C CXX)

add_executable(ScreenMirror
    source/main.c
)

wut_create_rpx(ScreenMirror)
