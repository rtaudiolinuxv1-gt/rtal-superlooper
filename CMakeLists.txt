cmake_minimum_required(VERSION 3.20)

project(SuperLooper
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

set(SUPERLOOPER_QT_VERSION "AUTO" CACHE STRING "Qt version to build with: AUTO, 5, or 6. AUTO prefers Qt5 when available; Qt6 support is untested.")
set_property(CACHE SUPERLOOPER_QT_VERSION PROPERTY STRINGS AUTO 5 6)

if(SUPERLOOPER_QT_VERSION STREQUAL "AUTO")
    find_package(QT NAMES Qt5 Qt6 REQUIRED COMPONENTS Widgets)
elseif(SUPERLOOPER_QT_VERSION STREQUAL "5")
    find_package(QT NAMES Qt5 REQUIRED COMPONENTS Widgets)
elseif(SUPERLOOPER_QT_VERSION STREQUAL "6")
    message(WARNING "Qt6 support is present but untested for SuperLooper.")
    find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets)
else()
    message(FATAL_ERROR "SUPERLOOPER_QT_VERSION must be AUTO, 5, or 6.")
endif()

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
if(QT_VERSION_MAJOR EQUAL 6)
    message(WARNING "Building with Qt6. This path is untested; Qt5 remains the primary supported build.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(AUBIO REQUIRED IMPORTED_TARGET aubio)
pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
pkg_check_modules(RTMIDI REQUIRED IMPORTED_TARGET rtmidi)
pkg_check_modules(SOXR QUIET IMPORTED_TARGET soxr)
pkg_check_modules(SAMPLERATE QUIET IMPORTED_TARGET samplerate)
pkg_check_modules(LILV QUIET IMPORTED_TARGET lilv-0)

option(SUPERLOOPER_WITH_SOXR "Build with libsoxr resampling support" ${SOXR_FOUND})
option(SUPERLOOPER_WITH_LIBSAMPLERATE "Build with libsamplerate resampling support" ${SAMPLERATE_FOUND})
option(SUPERLOOPER_WITH_LILV "Build with Lilv/LV2 plugin host support" ${LILV_FOUND})

if(SUPERLOOPER_WITH_SOXR AND NOT SOXR_FOUND)
    message(FATAL_ERROR "SUPERLOOPER_WITH_SOXR is ON, but pkg-config could not find soxr")
endif()

if(SUPERLOOPER_WITH_LIBSAMPLERATE AND NOT SAMPLERATE_FOUND)
    message(FATAL_ERROR "SUPERLOOPER_WITH_LIBSAMPLERATE is ON, but pkg-config could not find samplerate")
endif()

if(SUPERLOOPER_WITH_LILV AND NOT LILV_FOUND)
    message(FATAL_ERROR "SUPERLOOPER_WITH_LILV is ON, but pkg-config could not find lilv-0")
endif()

add_executable(SuperLooper
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/RubberBandWorker.h
    src/RubberBandWorker.cpp
    src/RootTimingWorker.h
    src/RootTimingWorker.cpp
    src/AudioEngine.h
    src/AudioEngine.cpp
    src/MidiInput.h
    src/MidiInput.cpp
    src/LoopManager.h
    src/LoopManager.cpp
    src/PedalBar.h
    src/PedalBar.cpp
    src/PianoWidget.h
    src/PianoWidget.cpp
    src/OptimizationLog.h
    src/OptimizationLog.cpp
    src/Sample.h
    src/Lv2Host.h
    src/Lv2Host.cpp
    src/SampleLoader.h
    src/SampleLoader.cpp
    src/SamplePoolWidget.h
    src/SamplePoolWidget.cpp
)

target_link_libraries(SuperLooper
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        PkgConfig::JACK
        PkgConfig::SNDFILE
        PkgConfig::AUBIO
        PkgConfig::RUBBERBAND
        PkgConfig::FFMPEG
        PkgConfig::RTMIDI
)

if(SUPERLOOPER_WITH_SOXR)
    target_link_libraries(SuperLooper PRIVATE PkgConfig::SOXR)
    target_compile_definitions(SuperLooper PRIVATE SUPERLOOPER_HAS_SOXR=1)
else()
    target_compile_definitions(SuperLooper PRIVATE SUPERLOOPER_HAS_SOXR=0)
endif()

if(SUPERLOOPER_WITH_LIBSAMPLERATE)
    target_link_libraries(SuperLooper PRIVATE PkgConfig::SAMPLERATE)
    target_compile_definitions(SuperLooper PRIVATE SUPERLOOPER_HAS_LIBSAMPLERATE=1)
else()
    target_compile_definitions(SuperLooper PRIVATE SUPERLOOPER_HAS_LIBSAMPLERATE=0)
endif()

if(SUPERLOOPER_WITH_LILV)
    target_link_libraries(SuperLooper PRIVATE PkgConfig::LILV)
    target_compile_definitions(SuperLooper PRIVATE SUPERLOOPER_HAS_LILV=1)
else()
    target_compile_definitions(SuperLooper PRIVATE SUPERLOOPER_HAS_LILV=0)
endif()

target_compile_options(SuperLooper
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

install(TARGETS SuperLooper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/superlooper.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/superlooper.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

include(CTest)

if(BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

    add_executable(test_loop_manager
        tests/test_loop_manager.cpp
        src/LoopManager.h
        src/LoopManager.cpp
        src/Lv2Host.h
        src/Lv2Host.cpp
        src/PianoWidget.h
        src/PianoWidget.cpp
    )

    target_link_libraries(test_loop_manager
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Test
            Qt${QT_VERSION_MAJOR}::Widgets
    )

    if(SUPERLOOPER_WITH_LILV)
        target_link_libraries(test_loop_manager PRIVATE PkgConfig::LILV)
        target_compile_definitions(test_loop_manager PRIVATE SUPERLOOPER_HAS_LILV=1)
    else()
        target_compile_definitions(test_loop_manager PRIVATE SUPERLOOPER_HAS_LILV=0)
    endif()

    target_compile_options(test_loop_manager
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    add_test(NAME LoopManagerTests COMMAND test_loop_manager)
    set_tests_properties(LoopManagerTests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()
