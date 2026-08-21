cmake_minimum_required(VERSION 3.25)
project(LogiPro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOGIPRO_BUILD_CLI "Build the optional command-line client" ON)
option(LOGIPRO_BUILD_GUI "Build the optional native Windows GUI" OFF)

include(CTest)
include(GNUInstallDirs)

add_library(logipro_core SHARED
    src/hid_windows.cpp
    src/hid_capture_windows.cpp
    src/raw_input_windows.cpp
    src/hidpp_windows.cpp
    src/hook_windows.cpp
)

target_include_directories(logipro_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(logipro_core PRIVATE LOGIPRO_BUILD_DLL)
target_compile_definitions(logipro_core INTERFACE LOGIPRO_USE_DLL)
set_target_properties(logipro_core PROPERTIES OUTPUT_NAME logipro PREFIX "")

if(MSVC)
    target_compile_options(logipro_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(logipro_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LOGIPRO_BUILD_CLI)
    add_executable(logipro
        src/app.cpp
        src/main.cpp
    )
    target_link_libraries(logipro PRIVATE logipro_core)
    install(TARGETS logipro RUNTIME DESTINATION bin)
endif()

if(WIN32)
    target_compile_definitions(logipro_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(logipro_core PRIVATE hid setupapi)
endif()

if(LOGIPRO_BUILD_GUI)
    if(NOT WIN32)
        message(FATAL_ERROR "LOGIPRO_BUILD_GUI requires Windows")
    endif()
    add_executable(logipro_gui WIN32
        gui/main_windows.cpp
    )
    target_compile_definitions(logipro_gui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(logipro_gui PRIVATE logipro_core user32)
    install(TARGETS logipro_gui RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING AND LOGIPRO_BUILD_CLI)
    add_test(NAME logipro_version COMMAND logipro --version)
endif()

install(TARGETS logipro_core
    EXPORT LogiProTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT LogiProTargets NAMESPACE LogiPro:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LogiPro)
