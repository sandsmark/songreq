cmake_minimum_required (VERSION 3.5)

project(songreq)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(PkgConfig)
pkg_check_modules(avcodec REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(avformat REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(avutil REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(swresample REQUIRED IMPORTED_TARGET libswresample)

find_package(zita-resampler) #avformat)

find_package(dlib REQUIRED)

add_executable(songreq
    main.cpp
    Fingerprinter.cpp
)
target_link_libraries(songreq
    PkgConfig::avcodec
    PkgConfig::avformat
    PkgConfig::avutil
    PkgConfig::swresample
    dlib::dlib
    ${ZITARESAMPLER_LIBRARIES}
)

target_include_directories(songreq
    PRIVATE
        ${ZITARESAMPLER_INCLUDE_DIR}
)


set(CMAKE_INCLUDE_CURRENT_DIR True)

