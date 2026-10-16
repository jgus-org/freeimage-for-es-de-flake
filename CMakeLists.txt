cmake_minimum_required(VERSION 3.28)

project(freeimage-for-es-de VERSION 3.18.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FI_SOURCES
  src/FreeImage.cpp
  src/abi.cpp
  src/dib.cpp
  src/io.cpp
  src/convert.cpp
  src/registry.cpp
  src/multipage.cpp
  src/resample.cpp
  src/codecs/bmp.cpp
  src/codecs/gif.cpp
  src/codecs/jpeg.cpp
  src/codecs/png.cpp
  src/codecs/webp.cpp
)

add_library(freeimage SHARED ${FI_SOURCES})

# src is on the include path for the internal headers (dib.h, codec.h, io.h, ...);
# the public header directory is exported to consumers.
target_include_directories(freeimage
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
         PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Codec backends. Optional at configure time so the header/registry skeleton
# builds before any codec reaches for a backend; each is used by its own TU.
# Each module is resolved on its own so one absent pkg-config file cannot drop
# the others: nixpkgs giflib ships no module at all and is passed to the linker
# by name.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FI_JPEG libjpeg)
pkg_check_modules(FI_PNG libpng16)
pkg_check_modules(FI_WEBP libwebp)
pkg_check_modules(FI_ZLIB zlib)

target_include_directories(freeimage PRIVATE
  ${FI_JPEG_INCLUDE_DIRS}
  ${FI_PNG_INCLUDE_DIRS}
  ${FI_WEBP_INCLUDE_DIRS}
  ${FI_ZLIB_INCLUDE_DIRS})
target_link_directories(freeimage PRIVATE
  ${FI_JPEG_LIBRARY_DIRS}
  ${FI_PNG_LIBRARY_DIRS}
  ${FI_WEBP_LIBRARY_DIRS}
  ${FI_ZLIB_LIBRARY_DIRS})
target_link_libraries(freeimage PRIVATE
  ${FI_JPEG_LINK_LIBRARIES}
  ${FI_PNG_LINK_LIBRARIES}
  ${FI_WEBP_LINK_LIBRARIES}
  ${FI_ZLIB_LINK_LIBRARIES}
  gif)

set_target_properties(freeimage PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

install(TARGETS freeimage LIBRARY DESTINATION lib)
install(FILES src/include/FreeImage.h DESTINATION include)

# The unit suite is opt-in so the package build stays production-only; the test
# build turns it on and runs it through the `check` target.
option(FREEIMAGE_FOR_ES_DE_TESTS "Build the doctest unit-test binary" OFF)

if(FREEIMAGE_FOR_ES_DE_TESTS)
  # New test files are picked up by the glob, so adding one needs no CMake edit.
  file(GLOB FI_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  # smoke.cpp, link_all.cpp and esde_surface.cpp are standalone flake checks, each
  # with its own main(); the unit suite's entry point is tests/helpers.cpp.
  list(FILTER FI_TEST_SOURCES EXCLUDE REGEX "/(smoke|link_all|esde_surface)\\.cpp$")

  pkg_check_modules(DOCTEST REQUIRED doctest)

  # tests/helpers_codecs.cpp calls the codec backends directly, so the test binary links
  # them itself. Each module is resolved on its own so one absent pkg-config file cannot
  # drop the others; giflib ships no module at all and is passed to the linker by name.
  pkg_check_modules(FI_TEST_JPEG libjpeg)
  pkg_check_modules(FI_TEST_PNG libpng16)
  pkg_check_modules(FI_TEST_WEBP libwebp)
  pkg_check_modules(FI_TEST_ZLIB zlib)

  # The library sources are compiled into the test binary rather than linked through
  # the shared object, so the binary runs from the build tree without an RPATH dance.
  add_executable(freeimage-for-es-de-tests ${FI_SOURCES} ${FI_TEST_SOURCES})
  target_include_directories(freeimage-for-es-de-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${DOCTEST_INCLUDE_DIRS}
            ${FI_TEST_JPEG_INCLUDE_DIRS}
            ${FI_TEST_PNG_INCLUDE_DIRS}
            ${FI_TEST_WEBP_INCLUDE_DIRS}
            ${FI_TEST_ZLIB_INCLUDE_DIRS})
  target_link_directories(freeimage-for-es-de-tests
    PRIVATE ${DOCTEST_LIBRARY_DIRS}
            ${FI_TEST_JPEG_LIBRARY_DIRS}
            ${FI_TEST_PNG_LIBRARY_DIRS}
            ${FI_TEST_WEBP_LIBRARY_DIRS}
            ${FI_TEST_ZLIB_LIBRARY_DIRS})
  target_link_libraries(freeimage-for-es-de-tests
    PRIVATE ${DOCTEST_LINK_LIBRARIES}
            ${FI_TEST_JPEG_LINK_LIBRARIES}
            ${FI_TEST_PNG_LINK_LIBRARIES}
            ${FI_TEST_WEBP_LINK_LIBRARIES}
            ${FI_TEST_ZLIB_LINK_LIBRARIES}
            gif)

  add_custom_target(check
    COMMAND freeimage-for-es-de-tests
    DEPENDS freeimage-for-es-de-tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
