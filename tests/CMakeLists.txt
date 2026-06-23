cmake_minimum_required(VERSION 3.14)
project(OpenmetricsFormatTests LANGUAGES CXX)

include(LogosTest)

# Pure unit tests for the OpenMetrics text <-> LogosMap layer (parse + render).
# Only openmetrics_format.cpp is under test — it depends on nothing but std and
# nlohmann::json (via <logos_json.h>), so no module/Qt/IPC machinery is needed.
logos_test(
    NAME openmetrics_format_tests
    MODULE_SOURCES
        ../src/openmetrics_format.cpp
    TEST_SOURCES
        main.cpp
        test_openmetrics_format.cpp
    EXTRA_INCLUDES
        ../src
)
