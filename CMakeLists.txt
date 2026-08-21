cmake_minimum_required(VERSION 3.25)
project(LogiPro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOGIPRO_BUILD_CLI "Build the optional command-line client" ON)
option(LOGIPRO_BUILD_GUI "Build the optional native Windows GUI" OFF)

include(CTest)
include(GNUInstallDirs)

set(LOGIPRO_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
set(LOGIPRO_LIB_DIR "${CMAKE_SOURCE_DIR}/lib")

function(logipro_set_output_directories target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${LOGIPRO_BIN_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${LOGIPRO_BIN_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${LOGIPRO_LIB_DIR}"
    )
    foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${configuration}" configuration_upper)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${configuration_upper} "${LOGIPRO_BIN_DIR}"
            LIBRARY_OUTPUT_DIRECTORY_${configuration_upper} "${LOGIPRO_BIN_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY_${configuration_upper} "${LOGIPRO_LIB_DIR}"
        )
    endforeach()
endfunction()

add_library(logipro_core SHARED
    src/hid/hid.cpp
    src/hid/capture.cpp
    src/input/raw_input.cpp
    src/hidpp/hidpp.cpp
    src/input/hook.cpp
)

target_include_directories(logipro_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(logipro_core PRIVATE LOGIPRO_BUILD_DLL)
target_compile_definitions(logipro_core INTERFACE LOGIPRO_USE_DLL)
set_target_properties(logipro_core PROPERTIES OUTPUT_NAME logipro PREFIX "")
logipro_set_output_directories(logipro_core)

if(MSVC)
    target_compile_options(logipro_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(logipro_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LOGIPRO_BUILD_CLI)
    add_executable(logipro
        src/cli/app.cpp
        src/cli/main.cpp
    )
    target_link_libraries(logipro PRIVATE logipro_core)
    logipro_set_output_directories(logipro)
    install(TARGETS logipro RUNTIME DESTINATION bin)
endif()

if(WIN32)
    target_compile_definitions(logipro_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(logipro_core PRIVATE hid setupapi)
endif()

if(LOGIPRO_BUILD_GUI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
    add_executable(logipro_gui
        gui/main.cpp
    )
    target_link_libraries(logipro_gui PRIVATE logipro_core PkgConfig::GTK4)
    logipro_set_output_directories(logipro_gui)
    install(TARGETS logipro_gui RUNTIME DESTINATION bin)
endif()

if(MINGW)
    target_link_options(logipro_core PRIVATE -static-libgcc -static-libstdc++)
    if(LOGIPRO_BUILD_CLI)
        target_link_options(logipro PRIVATE -static-libgcc -static-libstdc++)
    endif()
    if(LOGIPRO_BUILD_GUI)
        target_link_options(logipro_gui PRIVATE -static-libgcc -static-libstdc++)
    endif()
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
