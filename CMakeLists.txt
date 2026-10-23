cmake_minimum_required(VERSION 3.24)
project(lmimg VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
option(LMIMG_FULL_STATIC "Link the complete executable statically" ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imagemagick/.git")
  message(FATAL_ERROR "third_party/imagemagick missing: run scripts/sync-deps.sh")
endif()

# Build the codec closure into a private prefix.  ImageMagick is then pointed
# at these static archives through pkg-config; no host codec or runtime module
# is used by the resulting executable.
set(CODEC_PREFIX "${CMAKE_BINARY_DIR}/codec-prefix")
# Keep every generated .pc file in one directory. This avoids platform-specific
# path-list parsing in native pkg-config on Windows (where ':' is a drive
# separator and ';' is the list separator).
set(CODEC_PKGCONFIG_DIR "${CODEC_PREFIX}/lib/pkgconfig")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/codec-build")
function(build_codec name source opts)
  set(dir "${CMAKE_BINARY_DIR}/codec-build/${name}")
  set(stamp "${dir}/.configured")
  if(NOT EXISTS "${stamp}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${source}"
    -B "${dir}" -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    "-DCMAKE_INSTALL_PREFIX=${CODEC_PREFIX}" -DBUILD_SHARED_LIBS=OFF ${opts}
    RESULT_VARIABLE cfg)
    if(NOT cfg EQUAL 0)
      message(FATAL_ERROR "${name} configure failed (${cfg})")
    endif()
    file(TOUCH "${stamp}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${dir}" --parallel 2
    RESULT_VARIABLE build)
  if(NOT build EQUAL 0)
    message(FATAL_ERROR "${name} build failed (${build})")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --install "${dir}"
    RESULT_VARIABLE install)
  if(NOT install EQUAL 0)
    message(FATAL_ERROR "${name} install failed (${install})")
  endif()
endfunction()
build_codec(zlib zlib "-DZLIB_BUILD_TESTING=OFF")
if(WIN32)
  # zlib's CMake project keeps the static archive name `libzlibstatic.a`,
  # while ImageMagick's pkg-config probe and the link closure use -lz.
  # Provide that conventional alias without changing the upstream archive.
  set(ZLIB_WINDOWS_ARCHIVE "${CODEC_PREFIX}/lib/libzlibstatic.a")
  set(CODEC_ZLIB "${CODEC_PREFIX}/lib/libz.a")
  if(NOT EXISTS "${ZLIB_WINDOWS_ARCHIVE}")
    message(FATAL_ERROR "zlib static archive missing: ${ZLIB_WINDOWS_ARCHIVE}")
  endif()
  file(COPY_FILE "${ZLIB_WINDOWS_ARCHIVE}" "${CODEC_ZLIB}" ONLY_IF_DIFFERENT)
else()
  set(CODEC_ZLIB "${CODEC_PREFIX}/lib/libz.a")
endif()
set(ZLIB_PC_SOURCE "${CODEC_PREFIX}/share/pkgconfig/zlib.pc")
set(ZLIB_PC_TARGET "${CODEC_PREFIX}/lib/pkgconfig/zlib.pc")
if(NOT EXISTS "${ZLIB_PC_SOURCE}")
  message(FATAL_ERROR "zlib pkg-config metadata missing: ${ZLIB_PC_SOURCE}")
endif()
file(MAKE_DIRECTORY "${CODEC_PREFIX}/lib/pkgconfig")
file(COPY_FILE "${ZLIB_PC_SOURCE}" "${ZLIB_PC_TARGET}" ONLY_IF_DIFFERENT)
build_codec(libpng libpng "-DPNG_TESTS=OFF;-DPNG_SHARED=OFF;-DPNG_STATIC=ON;-DCMAKE_PREFIX_PATH=${CODEC_PREFIX}")
build_codec(libjpeg-turbo libjpeg-turbo "-DENABLE_SHARED=OFF;-DENABLE_STATIC=ON;-DWITH_TESTS=OFF;-DWITH_12BIT=OFF;-DWITH_16BIT=OFF")
build_codec(libtiff libtiff "-Dtiff-tests=OFF;-Djbig=OFF;-Dlerc=OFF;-Dlzma=OFF;-Dzstd=OFF;-Dlibdeflate=OFF;-DCMAKE_PREFIX_PATH=${CODEC_PREFIX}")
build_codec(libwebp libwebp "-DWEBP_BUILD_ANIM_UTILS=OFF;-DWEBP_BUILD_CWEBP=OFF;-DWEBP_BUILD_DWEBP=OFF;-DWEBP_BUILD_EXTRAS=OFF;-DWEBP_BUILD_GIF2WEBP=OFF;-DWEBP_BUILD_IMG2WEBP=OFF;-DWEBP_BUILD_VWEBP=OFF;-DWEBP_BUILD_WEBPINFO=OFF;-DWEBP_BUILD_WEBPMUX=OFF")

set(FONT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/roboto/src/hinted/Roboto-Regular.ttf")
if(NOT EXISTS "${FONT_FILE}")
  message(FATAL_ERROR "embedded Roboto font missing from pinned checkout")
endif()
file(READ "${FONT_FILE}" FONT_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FONT_HEX "${FONT_HEX}")
set(STB_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/stb_truetype.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/stb_truetype.h" "${STB_HEADER}" COPYONLY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_font.hpp"
  "#pragma once\n#include <cstddef>\nnamespace lmimg { inline constexpr unsigned char embedded_font[] = {${FONT_HEX}0}; inline constexpr std::size_t embedded_font_size = sizeof(embedded_font)-1; }\n")

# ImageMagick's supported build is Autoconf rather than CMake.  Configure it
# once into the build tree, with Q8/no-HDRI pixels and no loadable modules.
# All five codec delegates are required and verified after configuration.
set(IM_PREFIX "${CMAKE_BINARY_DIR}/imagemagick-prefix")
set(IM_BUILD "${CMAKE_BINARY_DIR}/imagemagick-build")
file(MAKE_DIRECTORY "${IM_BUILD}")
set(IM_CONFIGURED "${IM_BUILD}/.configured")
if(WIN32)
  find_program(IM_MAKE_COMMAND NAMES mingw32-make make REQUIRED)
  find_program(IM_BASH_COMMAND NAMES bash REQUIRED)
  find_program(IM_CYGPATH_COMMAND NAMES cygpath REQUIRED)
  execute_process(COMMAND "${IM_CYGPATH_COMMAND}" -u
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imagemagick/configure"
    OUTPUT_VARIABLE IM_CONFIGURE_SCRIPT OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE IM_CYGPATH_RESULT)
  if(NOT IM_CYGPATH_RESULT EQUAL 0 OR NOT IM_CONFIGURE_SCRIPT)
    message(FATAL_ERROR "cygpath could not translate the ImageMagick configure script")
  endif()
  execute_process(COMMAND "${IM_CYGPATH_COMMAND}" -u "${IM_PREFIX}"
    OUTPUT_VARIABLE IM_PREFIX_BASH OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE IM_CYGPATH_PREFIX_RESULT)
  if(NOT IM_CYGPATH_PREFIX_RESULT EQUAL 0 OR NOT IM_PREFIX_BASH)
    message(FATAL_ERROR "cygpath could not translate the ImageMagick install prefix")
  endif()
  execute_process(COMMAND "${IM_CYGPATH_COMMAND}" -u "${CODEC_PREFIX}"
    OUTPUT_VARIABLE CODEC_PREFIX_BASH OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE IM_CYGPATH_CODEC_PREFIX_RESULT)
  if(NOT IM_CYGPATH_CODEC_PREFIX_RESULT EQUAL 0 OR NOT CODEC_PREFIX_BASH)
    message(FATAL_ERROR "cygpath could not translate the codec prefix")
  endif()
  set(IM_PKG_CONFIG_NATIVE "${IM_BUILD}/pkg-config-codecs")
  execute_process(COMMAND "${IM_CYGPATH_COMMAND}" -u "${IM_PKG_CONFIG_NATIVE}"
    OUTPUT_VARIABLE IM_PKG_CONFIG OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE IM_CYGPATH_PKG_RESULT)
  if(NOT IM_CYGPATH_PKG_RESULT EQUAL 0 OR NOT IM_PKG_CONFIG)
    message(FATAL_ERROR "cygpath could not translate the static codec pkg-config bridge")
  endif()
  set(IM_CONFIGURE_COMMAND "${IM_BASH_COMMAND}" "${IM_CONFIGURE_SCRIPT}")
else()
  set(IM_MAKE_COMMAND make)
  set(IM_CONFIGURE_COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imagemagick/configure")
  find_program(IM_PKG_CONFIG NAMES pkg-config pkgconf REQUIRED)
endif()
set(IM_CONFIG_INPUTS
  "${CMAKE_CURRENT_SOURCE_DIR}/deps.lock"
  "${CMAKE_SYSTEM_NAME}|${CMAKE_SYSTEM_PROCESSOR}|${CMAKE_C_COMPILER_ID}|${CMAKE_CXX_COMPILER_ID}"
  "${CODEC_PKGCONFIG_DIR}|MAGICKCORE_HDRI_ENABLE=0|MAGICKCORE_QUANTUM_DEPTH=8|MAGICKCORE_CHANNEL_MASK_DEPTH=32|static-pkgconfig-v2|sharpyuv")
string(SHA256 IM_CONFIG_KEY "${IM_CONFIG_INPUTS}")
set(IM_NEEDS_CONFIGURE TRUE)
if(EXISTS "${IM_CONFIGURED}")
  file(READ "${IM_CONFIGURED}" IM_CONFIGURED_KEY)
  string(STRIP "${IM_CONFIGURED_KEY}" IM_CONFIGURED_KEY)
  if(IM_CONFIGURED_KEY STREQUAL IM_CONFIG_KEY)
    set(IM_NEEDS_CONFIGURE FALSE)
  else()
    file(REMOVE_RECURSE "${IM_BUILD}")
    file(MAKE_DIRECTORY "${IM_BUILD}")
  endif()
endif()
if(IM_NEEDS_CONFIGURE)
if(WIN32)
  execute_process(COMMAND "${IM_CYGPATH_COMMAND}" -u "${CODEC_PREFIX}/include"
    OUTPUT_VARIABLE CODEC_INCLUDE_BASH OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND "${IM_CYGPATH_COMMAND}" -u "${CODEC_PREFIX}/lib"
    OUTPUT_VARIABLE CODEC_LIB_BASH OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(IM_CPPFLAGS "-I${CODEC_INCLUDE_BASH}")
  set(IM_LDFLAGS "-L${CODEC_LIB_BASH}")
  set(IM_LIBS "-L${CODEC_LIB_BASH} -lsharpyuv")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/scripts/static-codec-pkg-config.sh.in"
    "${IM_PKG_CONFIG_NATIVE}" @ONLY)
  file(CHMOD "${IM_PKG_CONFIG_NATIVE}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)
  foreach(codec_query IN ITEMS "zlib >= 1.0.0" "libpng >= 1.0.0"
      "libjpeg >= 1.0.0" "libtiff-4 >= 4.0.0" "libwebp >= 0.4.1")
    execute_process(COMMAND "${IM_BASH_COMMAND}" "${IM_PKG_CONFIG}" --exists "${codec_query}"
      RESULT_VARIABLE CODEC_QUERY_RESULT ERROR_VARIABLE CODEC_QUERY_ERROR)
    if(NOT CODEC_QUERY_RESULT EQUAL 0)
      message(FATAL_ERROR "static codec pkg-config bridge rejected '${codec_query}': ${CODEC_QUERY_ERROR}")
    endif()
  endforeach()
  set(IM_PREFIX_CONFIGURE "${IM_PREFIX_BASH}")
else()
  set(IM_CPPFLAGS "-I${CODEC_PREFIX}/include")
  set(IM_LDFLAGS "-L${CODEC_PREFIX}/lib")
  set(IM_LIBS "-L${CODEC_PREFIX}/lib -lsharpyuv")
  set(IM_PREFIX_CONFIGURE "${IM_PREFIX}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "CPPFLAGS=${IM_CPPFLAGS}"
    "LDFLAGS=${IM_LDFLAGS}"
    "LIBS=${IM_LIBS}"
    "PKG_CONFIG=${IM_PKG_CONFIG}"
    "PKG_CONFIG_PATH=${CODEC_PKGCONFIG_DIR}"
    "PKG_CONFIG_LIBDIR=${CODEC_PKGCONFIG_DIR}"
    ${IM_CONFIGURE_COMMAND}
    "--prefix=${IM_PREFIX_CONFIGURE}" --disable-shared --enable-static
    --with-quantum-depth=8 --disable-hdri
    --without-modules --disable-openmp --without-x --without-perl
    --without-magick-plus-plus --without-xml --without-bzlib --without-djvu
    --without-fftw --without-fontconfig --without-gslib --without-heic
    --without-jxl --without-lcms --without-lqr --without-openexr
    --without-raqm --without-raw --without-wmf --without-x11
    --with-png=yes --with-jpeg=yes --with-tiff=yes --with-webp=yes
    --with-zlib=yes --without-freetype --without-zstd --without-lzma
    --without-jbig --without-openjp2 --without-brotli
  WORKING_DIRECTORY "${IM_BUILD}"
  RESULT_VARIABLE IM_CONFIGURE_RESULT)
if(NOT IM_CONFIGURE_RESULT EQUAL 0)
  message(FATAL_ERROR "ImageMagick configure failed (${IM_CONFIGURE_RESULT})")
endif()
file(WRITE "${IM_CONFIGURED}" "${IM_CONFIG_KEY}\n")
endif()

# A successful configure is not enough: ImageMagick treats optional delegates
# as soft failures.  Stop at configure time if one of the four formats that
# lmimg promises was omitted from the static MagickCore build.
set(IM_CONFIG_HEADER "${IM_BUILD}/config/config.h")
if(NOT EXISTS "${IM_CONFIG_HEADER}")
  message(FATAL_ERROR "ImageMagick config header missing: ${IM_CONFIG_HEADER}")
endif()
file(READ "${IM_CONFIG_HEADER}" IM_CONFIG_TEXT)
foreach(delegate PNG JPEG TIFF WEBP ZLIB)
  if(NOT IM_CONFIG_TEXT MATCHES "#define[ \t]+${delegate}_DELEGATE[ \t]+1")
    message(FATAL_ERROR "ImageMagick ${delegate} delegate was not detected; check pinned codec prefix and pkg-config")
  endif()
endforeach()
message(STATUS "ImageMagick delegates: PNG JPEG TIFF WEBP ZLIB (static)")
execute_process(COMMAND "${IM_MAKE_COMMAND}" -j2
  WORKING_DIRECTORY "${IM_BUILD}" RESULT_VARIABLE IM_MAKE_RESULT)
if(NOT IM_MAKE_RESULT EQUAL 0)
  message(FATAL_ERROR "ImageMagick build failed (${IM_MAKE_RESULT})")
endif()
execute_process(COMMAND "${IM_MAKE_COMMAND}" install
  WORKING_DIRECTORY "${IM_BUILD}" RESULT_VARIABLE IM_INSTALL_RESULT)
if(NOT IM_INSTALL_RESULT EQUAL 0)
  message(FATAL_ERROR "ImageMagick install failed (${IM_INSTALL_RESULT})")
endif()

find_library(IM_CORE_LIBRARY NAMES MagickCore-7.Q8 MagickCore-7.Q8HDRI
  PATHS "${IM_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(IM_WAND_LIBRARY NAMES MagickWand-7.Q8 MagickWand-7.Q8HDRI
  PATHS "${IM_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
add_library(lmimg_magick_core STATIC IMPORTED GLOBAL)
set_target_properties(lmimg_magick_core PROPERTIES IMPORTED_LOCATION "${IM_CORE_LIBRARY}")
add_library(lmimg_magick_wand STATIC IMPORTED GLOBAL)
set_target_properties(lmimg_magick_wand PROPERTIES IMPORTED_LOCATION "${IM_WAND_LIBRARY}")
find_package(Threads REQUIRED)
if(NOT EXISTS "${CODEC_ZLIB}")
  message(FATAL_ERROR "static zlib archive missing: ${CODEC_ZLIB}")
endif()
find_library(CODEC_PNG NAMES png16 png PATHS "${CODEC_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(CODEC_JPEG NAMES jpeg PATHS "${CODEC_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(CODEC_TIFF NAMES tiff PATHS "${CODEC_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(CODEC_WEBP NAMES webp PATHS "${CODEC_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(CODEC_WEBPDEMUX NAMES webpdemux PATHS "${CODEC_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(CODEC_WEBPMUX NAMES webpmux PATHS "${CODEC_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(CODEC_SHARPYUV NAMES sharpyuv PATHS "${CODEC_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
add_library(lmimg_magick INTERFACE)
target_compile_definitions(lmimg_magick INTERFACE
  MAGICKCORE_HDRI_ENABLE=0 MAGICKCORE_QUANTUM_DEPTH=8 MAGICKCORE_CHANNEL_MASK_DEPTH=32)
target_link_libraries(lmimg_magick INTERFACE
  lmimg_magick_wand lmimg_magick_core
  ${CODEC_TIFF} ${CODEC_JPEG} ${CODEC_PNG}
  ${CODEC_WEBPMUX} ${CODEC_WEBPDEMUX} ${CODEC_WEBP} ${CODEC_SHARPYUV}
  ${CODEC_ZLIB} Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(lmimg_magick INTERFACE m)
endif()
if(WIN32)
  # ImageMagick's MinGW configure probe records these native drawing and
  # window API imports in its static archive closure.
  target_link_libraries(lmimg_magick INTERFACE gdi32 user32)
endif()
target_include_directories(lmimg_magick INTERFACE
  "${IM_PREFIX}/include/ImageMagick-7" "${IM_PREFIX}/include")

function(embed_file var file ns name)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
  file(READ "${file}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  set(${var} "inline constexpr unsigned char ${name}[] = {${hex}0}; inline constexpr std::size_t ${name}_size = sizeof(${name}) - 1;\n" PARENT_SCOPE)
endfunction()
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN}")
embed_file(SCHEMA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/schema/lmimg.dsl.json" lmimg embedded_schema)
file(WRITE "${GEN}/schema_embedded.hpp" "#pragma once\n#include <cstddef>\nnamespace lmimg { ${SCHEMA_SRC} }\n")
embed_file(NOTICES_SRC "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES" lmimg embedded_notices)
file(WRITE "${GEN}/notices_embedded.hpp" "#pragma once\n#include <cstddef>\nnamespace lmimg { ${NOTICES_SRC} }\n")

if((CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") AND NOT APPLE)
  target_link_options(lmimg_magick INTERFACE -static-libstdc++ -static-libgcc)
  if(LMIMG_FULL_STATIC)
    target_link_options(lmimg_magick INTERFACE -static)
  endif()
endif()

# Main executable is supplied by the host implementation in cpp/main.cpp.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cpp/main.cpp")
  add_executable(lmimg cpp/main.cpp cpp/image.cpp cpp/memory_budget.cpp)
  target_compile_definitions(lmimg PRIVATE LMIMG_VERSION="${PROJECT_VERSION}")
  target_include_directories(lmimg PRIVATE cpp "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(lmimg PRIVATE lmimg_magick)
  target_compile_options(lmimg PRIVATE -Wall -Wextra)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT APPLE)
    target_link_options(lmimg PRIVATE -s)
  endif()
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/scientific.cpp")
  add_executable(lmimg-scientific tests/scientific.cpp cpp/image.cpp cpp/memory_budget.cpp)
  target_compile_definitions(lmimg-scientific PRIVATE LMIMG_VERSION="${PROJECT_VERSION}")
  target_include_directories(lmimg-scientific PRIVATE cpp "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(lmimg-scientific PRIVATE lmimg_magick)
  target_compile_options(lmimg-scientific PRIVATE -Wall -Wextra)
endif()

if(TARGET lmimg-scientific)
  enable_testing()
  add_test(NAME lmimg-scientific
    COMMAND lmimg-scientific "${CMAKE_CURRENT_BINARY_DIR}/scientific-output")
endif()
