cmake_minimum_required(VERSION 3.21)
project(SkyrimPluginFormat VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# bsa-reader (GPL-3.0, github.com/cecell/bsa-reader): strings packed in BSAs,
# compressed ones too. Pinned to the commit of tag v0.1.1; library changes go
# to that repo, then bump this pin. Fetched before the project-wide flags so
# it builds with its own.
include(FetchContent)
FetchContent_Declare(bsa_reader
    GIT_REPOSITORY https://github.com/cecell/bsa-reader.git
    GIT_TAG bbff0cbc5cd04d39bb3b50c470c579669c027dbf)  # v0.1.1
FetchContent_MakeAvailable(bsa_reader)

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
target_link_libraries(spf PUBLIC sqlite3 PRIVATE bsa::reader)

add_executable(spf_cli tools/spf.cpp)
set_target_properties(spf_cli PROPERTIES OUTPUT_NAME spf)
target_link_libraries(spf_cli PRIVATE spf)
# spf.exe looks for patterns.json next to itself. Copied whenever the catalog
# changes, not only when spf.exe relinks.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/patterns.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json
            ${CMAKE_CURRENT_BINARY_DIR}/patterns.json
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json)
add_custom_target(spf_patterns ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/patterns.json)

# Unit tests: hand-built valid and damaged plugins / strings files.
enable_testing()
add_executable(spf_tests tests/test_spf.cpp)
target_link_libraries(spf_tests PRIVATE spf)
add_test(NAME unit COMMAND spf_tests ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json)

# Fuzzing (separate build dir): cmake -B build-fuzz -DSPF_FUZZ=ON ...
# The library gets coverage + AddressSanitizer instrumentation; spf_fuzz links
# libFuzzer's main. Needs MSVC's clang_rt.asan_dynamic DLL on PATH (vcvars).
option(SPF_FUZZ "Build the libFuzzer target spf_fuzz" OFF)
if(SPF_FUZZ)
    # Everything linked into spf_fuzz must agree on ASan (container
    # annotations). Coverage-instrumented code needs libFuzzer's runtime, so
    # in a fuzz build only spf_fuzz links: build that target alone.
    foreach(t spf bsa_reader)
        target_compile_options(${t} PRIVATE /Zi /fsanitize=address /fsanitize-coverage=inline-8bit-counters
            /fsanitize-coverage=edge /fsanitize-coverage=trace-cmp)
    endforeach()
    add_executable(spf_fuzz tests/fuzz_plugin.cpp)
    target_compile_options(spf_fuzz PRIVATE /Zi /fsanitize=address /fsanitize=fuzzer)
    target_link_libraries(spf_fuzz PRIVATE spf)
    target_link_options(spf_fuzz PRIVATE /INCREMENTAL:NO /DEBUG)
    add_custom_command(TARGET spf_fuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json
                $<TARGET_FILE_DIR:spf_fuzz>/patterns.json)
endif()
