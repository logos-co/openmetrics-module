cmake_minimum_required(VERSION 3.14)
project(OpenmetricsModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. LOGOS_MODULE_BUILDER_ROOT is not set.")
endif()

# Universal module. The builder runs `logos-cpp-generator --from-header` for the
# Qt glue and, because metadata.json declares `interface_dependencies`,
# `--interface metrics_source=interfaces/metrics_source.h=IMetricsSource` to
# produce the bound `MetricsSource` wrapper + `bind_metrics_source` factory.
logos_module(
    NAME openmetrics
    SOURCES
        src/openmetrics_impl.h
        src/openmetrics_impl.cpp
        src/openmetrics_format.h
        src/openmetrics_format.cpp
)

# Embedded HTTP server: libmicrohttpd (the same server prometheus-cpp's exposer
# uses). Resolved via pkg-config; libmicrohttpd is provided as a build input by
# metadata.json's nix.packages.runtime.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MHD REQUIRED IMPORTED_TARGET libmicrohttpd)
target_link_libraries(openmetrics_module_plugin PRIVATE PkgConfig::MHD)
