cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(DEFINED ENV{APP_VERSION})
    set(PROJECT_VER "$ENV{APP_VERSION}")
endif()

project(xauusd_chart)
