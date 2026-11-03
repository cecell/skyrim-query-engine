cmake_minimum_required(VERSION 3.21)
project(SkyrimPluginFormat VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/utf-8 "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/EHsc>")

# SQLite (public domain amalgamation, third_party/sqlite, 3.54.1). Only the
# writer thread touches a connection, hence THREADSAFE=0.
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=0 SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_ENABLE_FTS5 SQLITE_ENABLE_MATH_FUNCTIONS SQLITE_OMIT_DEPRECATED)
target_compile_options(sqlite3 PRIVATE /W0)

# The library: pattern engine (decodes subrecords from data/patterns.json),
# zlib inflate, localized strings, and the plugin / record / field objects.
add_library(spf STATIC
    src/pattern_value.cpp
    src/pattern_engine.cpp
    src/inflate.cpp
    src/strings.cpp
    src/plugin.cpp
    src/db_schema.cpp
    src/db_build.cpp)
target_include_directories(spf PUBLIC src)
target_link_libraries(spf PUBLIC sqlite3)

add_executable(spf_cli tools/spf.cpp)
set_target_properties(spf_cli PROPERTIES OUTPUT_NAME spf)
target_link_libraries(spf_cli PRIVATE spf)
# spf.exe looks for patterns.json next to itself.
add_custom_command(TARGET spf_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json
            $<TARGET_FILE_DIR:spf_cli>/patterns.json)
