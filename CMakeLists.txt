cmake_minimum_required(VERSION 3.21)
project(SkyrimQueryEngine VERSION 0.92 LANGUAGES C CXX)

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
# beth-asset-metadata (GPL-3.0, github.com/cecell/beth-asset-metadata): DDS,
# TGA, NIF (header and texture paths), WAV / XWM / FUZ header facts for
# build-assets. Pinned to the commit of tag v0.3.1; changes go to that repo,
# then bump this pin.
FetchContent_Declare(asset_metadata
    GIT_REPOSITORY https://github.com/cecell/beth-asset-metadata.git
    GIT_TAG 45305c9de235260283211444b12bd5da9e10eba8)  # v0.3.1
FetchContent_MakeAvailable(asset_metadata)

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
# zlib inflate, localized strings, the plugin / record / field objects, and
# the database builders (build-db, build-assets).
add_library(skqe STATIC
    src/pattern_value.cpp
    src/pattern_engine.cpp
    src/inflate.cpp
    src/strings.cpp
    src/plugin.cpp
    src/db_schema.cpp
    src/db_build.cpp
    src/itm.cpp
    src/asset_readers.cpp
    src/asset_build.cpp)
target_include_directories(skqe PUBLIC src)
target_link_libraries(skqe PUBLIC sqlite3 PRIVATE bsa::reader asset::metadata bcrypt shell32 ole32)

add_executable(skqe_cli tools/skqe.cpp)
set_target_properties(skqe_cli PROPERTIES OUTPUT_NAME skqe)
target_link_libraries(skqe_cli PRIVATE skqe)
target_compile_definitions(skqe_cli PRIVATE SKQE_VERSION="${PROJECT_VERSION}")
# skqe.exe looks for patterns.json next to itself. Copied whenever the catalog
# changes, not only when skqe.exe relinks.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/patterns.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json
            ${CMAKE_CURRENT_BINARY_DIR}/patterns.json
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json)
add_custom_target(skqe_patterns ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/patterns.json)

# Unit tests: hand-built valid and damaged plugins / strings files.
enable_testing()
add_executable(skqe_tests tests/test_skqe.cpp)
target_link_libraries(skqe_tests PRIVATE skqe)
add_test(NAME unit COMMAND skqe_tests ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json)

# The plugin writer: an editable model over a plugin's bytes, field edits, and
# skqeedit.exe. A separate library and exe, so skqe.exe stays read-only and
# whoever links `skqe` gets neither it nor zlib. Built when this is the top-level
# project, or on request (-DSKQE_EDIT=ON).
option(SKQE_EDIT "Build the plugin writer" ${PROJECT_IS_TOP_LEVEL})
if(SKQE_EDIT)
    # zlib for deflate (skqe's own inflate covers reading). Only the sources
    # deflate needs, pinned to v1.3.1.
    FetchContent_Declare(zlib_src
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf  # v1.3.1
        SOURCE_SUBDIR do-not-configure)
    FetchContent_MakeAvailable(zlib_src)
    add_library(skqe_zlib STATIC
        ${zlib_src_SOURCE_DIR}/adler32.c ${zlib_src_SOURCE_DIR}/compress.c ${zlib_src_SOURCE_DIR}/crc32.c
        ${zlib_src_SOURCE_DIR}/deflate.c ${zlib_src_SOURCE_DIR}/trees.c ${zlib_src_SOURCE_DIR}/zutil.c)
    target_include_directories(skqe_zlib PUBLIC ${zlib_src_SOURCE_DIR})
    target_compile_definitions(skqe_zlib PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    target_compile_options(skqe_zlib PRIVATE /W0)

    add_library(skqe_edit STATIC
        src/edit/record_body.cpp
        src/edit/document.cpp
        src/edit/roundtrip.cpp
        src/edit/ops.cpp)
    target_link_libraries(skqe_edit PUBLIC skqe PRIVATE skqe_zlib)

    add_executable(skqeedit tools/skqeedit.cpp)
    target_link_libraries(skqeedit PRIVATE skqe_edit)
    add_dependencies(skqeedit skqe_patterns)
    # skqeedit.exe also reads these next to itself.
    foreach(f field_order.csv catalog_open.csv)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${f}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/${f} ${CMAKE_CURRENT_BINARY_DIR}/${f}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/${f})
        list(APPEND SKQE_EDIT_DATA ${CMAKE_CURRENT_BINARY_DIR}/${f})
    endforeach()
    add_custom_target(skqe_edit_data ALL DEPENDS ${SKQE_EDIT_DATA})

    add_executable(skqeedit_tests tests/test_edit.cpp)
    target_link_libraries(skqeedit_tests PRIVATE skqe_edit)
    add_test(NAME edit COMMAND skqeedit_tests ${CMAKE_CURRENT_SOURCE_DIR}/data)
endif()

# Fuzzing (separate build dir): cmake -B build-fuzz -DSKQE_FUZZ=ON ...
# The library gets coverage + AddressSanitizer instrumentation; skqe_fuzz links
# libFuzzer's main. Needs MSVC's clang_rt.asan_dynamic DLL on PATH (vcvars).
option(SKQE_FUZZ "Build the libFuzzer target skqe_fuzz" OFF)
if(SKQE_FUZZ)
    # Everything linked into skqe_fuzz must agree on ASan (container
    # annotations). Coverage-instrumented code needs libFuzzer's runtime, so
    # in a fuzz build only skqe_fuzz links: build that target alone.
    foreach(t skqe skqe_edit bsa_reader asset_metadata)
        target_compile_options(${t} PRIVATE /Zi /fsanitize=address /fsanitize-coverage=inline-8bit-counters
            /fsanitize-coverage=edge /fsanitize-coverage=trace-cmp)
    endforeach()
    add_executable(skqe_fuzz tests/fuzz_plugin.cpp)
    target_compile_options(skqe_fuzz PRIVATE /Zi /fsanitize=address /fsanitize=fuzzer)
    target_link_libraries(skqe_fuzz PRIVATE skqe skqe_edit)
    target_link_options(skqe_fuzz PRIVATE /INCREMENTAL:NO /DEBUG)
    add_custom_command(TARGET skqe_fuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/patterns.json
                $<TARGET_FILE_DIR:skqe_fuzz>/patterns.json)
endif()
