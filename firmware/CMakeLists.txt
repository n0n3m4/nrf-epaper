cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(espaper_nrf)

target_sources(app PRIVATE
  src/main.cpp
  src/ble.c
  src/epd.c
  src/miniz/miniz.c
  src/image_receiver.cpp
  src/crc32.cpp
)
target_include_directories(app PRIVATE src src/miniz)

# miniz is here for tinfl only; everything else is compiled out.
target_compile_definitions(app PRIVATE
  MINIZ_NO_DEFLATE_APIS
  MINIZ_NO_ARCHIVE_APIS
  MINIZ_NO_ZLIB_APIS
  MINIZ_NO_STDIO
  MINIZ_NO_TIME
  MINIZ_NO_MALLOC
)
