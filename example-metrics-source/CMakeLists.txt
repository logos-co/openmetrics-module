cmake_minimum_required(VERSION 3.14)
project(MetricsDemoModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. LOGOS_MODULE_BUILDER_ROOT is not set.")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/metadata.json
               ${CMAKE_CURRENT_BINARY_DIR}/metadata.json COPYONLY)

logos_module(
    NAME metrics_demo
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

find_library(LIBMETRICS_DEMO
    NAMES libmetrics_demo.a metrics_demo
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/lib
    NO_DEFAULT_PATH
)

if(NOT LIBMETRICS_DEMO)
    message(FATAL_ERROR
        "Rust static library (libmetrics_demo.a) not found in ${CMAKE_CURRENT_SOURCE_DIR}/lib. "
        "Ensure the preConfigure hook ran 'cargo build --release'."
    )
endif()

message(STATUS "Found Rust static library: ${LIBMETRICS_DEMO}")
target_link_libraries(metrics_demo_module_plugin PRIVATE ${LIBMETRICS_DEMO})

if(APPLE)
    target_link_libraries(metrics_demo_module_plugin PRIVATE
        "-framework CoreFoundation"
        "-framework Security"
    )
else()
    target_link_libraries(metrics_demo_module_plugin PRIVATE pthread dl)
endif()
