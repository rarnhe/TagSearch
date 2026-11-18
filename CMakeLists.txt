cmake_minimum_required(VERSION 3.20)

project(TagSearch LANGUAGES CXX)

add_executable(TagSearch WIN32
    TagSearch.cpp
    TagSearch.h
    TagSearchCore.cpp
    TagSearchCore.h
)

target_compile_features(TagSearch PRIVATE cxx_std_20)
target_compile_definitions(TagSearch PRIVATE UNICODE _UNICODE)
target_link_libraries(TagSearch PRIVATE
    comctl32
    propsys
    shlwapi
    shell32
    ole32
    oleaut32
    uuid
    windowsapp
)

if(MSVC)
    target_compile_options(TagSearch PRIVATE /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
