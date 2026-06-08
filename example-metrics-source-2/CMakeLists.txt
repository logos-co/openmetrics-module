cmake_minimum_required(VERSION 3.14)
project(MetricsDemo2ModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. LOGOS_MODULE_BUILDER_ROOT is not set.")
endif()

logos_module(
    NAME metrics_demo_2
    SOURCES
        src/metrics_demo_2_impl.h
        src/metrics_demo_2_impl.cpp
)
