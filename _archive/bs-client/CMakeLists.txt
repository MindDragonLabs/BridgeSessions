add_library(bs-client-core STATIC
    src/host_config.cpp
)

target_include_directories(bs-client-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(bs-client
    src/main.cpp
    src/terminal_raw.cpp
    src/keygen.cpp
    src/image_render.cpp
)
if(APPLE)
    target_sources(bs-client PRIVATE src/clipboard_bridge.mm)
elseif(WIN32)
    target_sources(bs-client PRIVATE src/clipboard_windows.cpp)
else()
    target_sources(bs-client PRIVATE src/clipboard_linux.cpp)
endif()

target_link_libraries(bs-client PRIVATE bs-client-core)

# macOS: compile as Objective-C++ for NSPasteboard clipboard bridge
if(APPLE)
    set_source_files_properties(src/main.cpp PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()

target_include_directories(bs-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../bs-protocol/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../bs-transport/include
)

target_link_libraries(bs-client PRIVATE
    bs-transport
    OpenSSL::SSL
    OpenSSL::Crypto
    ${ZSTD_TARGET}
)

if(nlohmann_json_FOUND)
    target_link_libraries(bs-client PRIVATE nlohmann_json::nlohmann_json)
endif()

if(APPLE)
    target_link_libraries(bs-client PRIVATE "-framework AppKit")
endif()

if(CLI11_FOUND)
    target_link_libraries(bs-client PRIVATE CLI11::CLI11)
endif()

add_executable(bs-client-tests
    tests/host_config_test.cpp
)

target_link_libraries(bs-client-tests PRIVATE
    bs-client-core
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(bs-client-tests)
