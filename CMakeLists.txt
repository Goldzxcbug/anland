cmake_minimum_required(VERSION 3.10)
project(anland C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")

add_library(socket_utils STATIC common/socket_utils.c)

if(ANDROID)
    add_executable(display_daemon daemon/daemon.c)
    target_link_libraries(display_daemon socket_utils)

    add_library(display_consumer SHARED libdisplay_consumer/display_consumer.c)
    target_link_libraries(display_consumer socket_utils)
else()
    add_executable(display_daemon daemon/daemon.c)
    target_link_libraries(display_daemon socket_utils pthread)

    add_library(display_producer SHARED libdisplay_producer/display_producer.c)
    target_link_libraries(display_producer socket_utils pthread)

    add_library(display_consumer SHARED libdisplay_consumer/display_consumer.c)
    target_link_libraries(display_consumer socket_utils pthread)

    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 sdl2)
        pkg_check_modules(GBM gbm)
        pkg_check_modules(LIBDRM libdrm)
        pkg_check_modules(EGL egl)
        pkg_check_modules(GLESV2 glesv2)
    endif()
    if(SDL2_FOUND AND GBM_FOUND AND LIBDRM_FOUND AND EGL_FOUND AND GLESV2_FOUND)
        add_executable(test_sdl_consumer tests/test_sdl_consumer.c)
        target_include_directories(test_sdl_consumer PRIVATE
            ${SDL2_INCLUDE_DIRS} ${GBM_INCLUDE_DIRS} ${LIBDRM_INCLUDE_DIRS}
            ${EGL_INCLUDE_DIRS} ${GLESV2_INCLUDE_DIRS})
        target_link_libraries(test_sdl_consumer display_consumer socket_utils pthread
            ${SDL2_LIBRARIES} ${GBM_LIBRARIES} ${LIBDRM_LIBRARIES}
            ${EGL_LIBRARIES} ${GLESV2_LIBRARIES})
    endif()
endif()
