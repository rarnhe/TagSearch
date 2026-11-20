cmake_minimum_required(VERSION 3.20)

project(TagSearch LANGUAGES CXX)

get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(isMultiConfig)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
endif()

add_library(TagSearchCore STATIC TagSearch_Core.cpp TagSearch_Core.h TagSearch_Layout.cpp TagSearch_Layout.h)
target_include_directories(TagSearchCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(TagSearchCore PUBLIC cxx_std_20)
target_compile_definitions(TagSearchCore PUBLIC UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(TagSearchCore PUBLIC shell32 ole32 propsys uuid structuredquery)

add_executable(TagSearch_TreeView WIN32 TagSearch_TreeView.cpp)
target_link_libraries(TagSearch_TreeView PRIVATE TagSearchCore comctl32 shlwapi)

add_executable(TagSearch_Caption WIN32 TagSearch_Caption.cpp)
target_link_libraries(TagSearch_Caption PRIVATE TagSearchCore dwmapi)

if(MSVC)
    target_compile_options(TagSearchCore PRIVATE /utf-8)
    target_compile_options(TagSearch_TreeView PRIVATE /utf-8)
    target_compile_options(TagSearch_Caption PRIVATE /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
