cmake_minimum_required(VERSION 3.20)

# Backend targets live under the top-level project.

add_library(edge_sqlite3 STATIC
  src/sqlite3.c
)
target_include_directories(edge_sqlite3 PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/src
)

add_library(edge_storage STATIC
  src/StorageEngine.cpp
)
target_include_directories(edge_storage PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/src
)
target_link_libraries(edge_storage PUBLIC edge_sqlite3)

# Small local tool: reads one JSON per line from stdin and inserts into SQLite.
add_executable(edge_ingest
  src/ingest_stdin.cpp
)
target_link_libraries(edge_ingest PRIVATE edge_storage)
target_include_directories(edge_ingest PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/src
)

option(EDGE_BUILD_SERVER "Build HTTP ingest server (edge_server)" OFF)

if(EDGE_BUILD_SERVER)
  # yaml-cpp for config parsing
  find_package(yaml-cpp REQUIRED)

  # OpenSSL for HTTPS calls to the DeepSeek API (cpp-httplib SSLClient).
  # On macOS with Homebrew: set OPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 if not found.
  find_package(OpenSSL REQUIRED)

  # HTTP server: POST /api/ingest -> SQLite inserts
  add_executable(edge_server
    src/main.cpp
    src/DataIngestor.cpp
    src/DataFilter.cpp
    src/AIQueryDispatcher.cpp
    src/ConfigLoader.cpp
    src/CloudSync.cpp
    src/httplib.h
    src/json.hpp
  )
  target_link_libraries(edge_server PRIVATE edge_storage yaml-cpp::yaml-cpp
    OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(edge_server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_include_directories(edge_server PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
  )

  if(WIN32)
    # cpp-httplib uses Winsock on Windows.
    target_link_libraries(edge_server PRIVATE ws2_32)
  endif()

  if(APPLE)
    # cpp-httplib loads system TLS certs from the macOS keychain.
    target_link_libraries(edge_server PRIVATE
      "-framework CoreFoundation" "-framework Security")
  endif()
endif()

if(MSVC)
  target_compile_options(edge_sqlite3 PRIVATE /W3)
  target_compile_options(edge_storage PRIVATE /W4)
  target_compile_options(edge_ingest PRIVATE /W4)
  if(EDGE_BUILD_SERVER)
    target_compile_options(edge_server PRIVATE /W4)
  endif()
else()
  target_compile_options(edge_sqlite3 PRIVATE -Wall -Wextra)
  target_compile_options(edge_storage PRIVATE -Wall -Wextra)
  target_compile_options(edge_ingest PRIVATE -Wall -Wextra)
  if(EDGE_BUILD_SERVER)
    target_compile_options(edge_server PRIVATE -Wall -Wextra)
  endif()
endif()
