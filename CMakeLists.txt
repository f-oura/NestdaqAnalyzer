cmake_minimum_required(VERSION 3.16)
project(NestdaqAnalyzer CXX)

# The code is C++17. ROOT's targets raise the standard if ROOT itself was built with a newer one.
if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------------------
# Options and external locations (no machine-specific defaults)
# ------------------------------------------------------------------------------
set(NESTDAQ_USER_IMPL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/nestdaq-user-impl"
    CACHE PATH "nestdaq-user-impl source directory (data format headers)")
set(NDA_BUILD_DEVICES "AUTO" CACHE STRING "Build the FairMQ devices: AUTO (if FairMQ is found), ON or OFF")
set_property(CACHE NDA_BUILD_DEVICES PROPERTY STRINGS AUTO ON OFF)
option(NDA_BUILD_LEGACY "Build the original OnlineAnalysisSlicer and OnlineAnalysisTFBuilder devices" OFF)
set(NESTDAQ_UNPACKER_SRC_DIR "" CACHE PATH "NestdaqUnpacker src directory (only for NDA_BUILD_LEGACY)")
option(NDA_BUILD_TESTS "Build the unit tests" ON)

if(NOT EXISTS "${NESTDAQ_USER_IMPL_DIR}/TimeFrameHeader.h")
  message(FATAL_ERROR "nestdaq-user-impl headers not found in ${NESTDAQ_USER_IMPL_DIR}. "
                      "Run 'git submodule update --init' or set NESTDAQ_USER_IMPL_DIR.")
endif()

set(NDA_WARNINGS -Wall -Wextra)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ------------------------------------------------------------------------------
# Core: data format (no ROOT, no FairMQ)
# ------------------------------------------------------------------------------
add_library(nda_format STATIC
  core/format/Frames.cc
  core/format/FileReader.cc
)
target_include_directories(nda_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")
# The nestdaq-user-impl headers use anonymous structs in unions; treat them as system headers.
target_include_directories(nda_format SYSTEM PUBLIC "${NESTDAQ_USER_IMPL_DIR}")
target_compile_options(nda_format PRIVATE ${NDA_WARNINGS})

# ------------------------------------------------------------------------------
# Core: monitor framework and generic modules (ROOT histograms, no FairMQ)
# ------------------------------------------------------------------------------
find_package(ROOT REQUIRED COMPONENTS Core RIO Hist RHTTP)

add_library(nda_monitor STATIC
  core/monitor/HistRegistry.cc
  core/monitor/Engine.cc
  core/modules/FemTable.cc
  core/modules/DaqHealthModule.cc
  core/modules/ChannelModule.cc
)
target_link_libraries(nda_monitor PUBLIC nda_format ROOT::Core ROOT::RIO ROOT::Hist ROOT::RHTTP)
target_compile_options(nda_monitor PRIVATE ${NDA_WARNINGS})

# ------------------------------------------------------------------------------
# Applications that read files (no FairMQ)
# ------------------------------------------------------------------------------
add_executable(nda-replay apps/nda-replay.cc)
target_link_libraries(nda-replay PRIVATE nda_monitor)
target_compile_options(nda-replay PRIVATE ${NDA_WARNINGS})
install(TARGETS nda-replay RUNTIME DESTINATION bin)

# ------------------------------------------------------------------------------
# FairMQ devices
# ------------------------------------------------------------------------------
set(_nda_need_fairmq OFF)
if(NDA_BUILD_DEVICES STREQUAL "ON" OR NDA_BUILD_LEGACY)
  find_package(FairMQ REQUIRED)
  set(_nda_need_fairmq ON)
elseif(NDA_BUILD_DEVICES STREQUAL "AUTO")
  find_package(FairMQ QUIET)
  if(FairMQ_FOUND)
    set(_nda_need_fairmq ON)
  else()
    message(STATUS "FairMQ not found: the FairMQ devices are not built (nda-replay only)")
  endif()
endif()

if(_nda_need_fairmq)
  # Dependencies of FairMQ (same pattern as the original NestdaqAnalyzer build).
  foreach(dep IN LISTS FairMQ_PACKAGE_DEPENDENCIES)
    if(FairMQ_${dep}_COMPONENTS)
      if(${dep} MATCHES "Boost")
        find_package(${dep} ${FairMQ_${dep}_VERSION} COMPONENTS ${FairMQ_${dep}_COMPONENTS} program_options)
      else()
        find_package(${dep} ${FairMQ_${dep}_VERSION} COMPONENTS ${FairMQ_${dep}_COMPONENTS})
      endif()
    else()
      find_package(${dep} ${FairMQ_${dep}_VERSION})
    endif()
  endforeach()
  if(NOT TARGET FairLogger::FairLogger)
    find_package(FairLogger REQUIRED)
  endif()
endif()

if(_nda_need_fairmq AND NOT NDA_BUILD_DEVICES STREQUAL "OFF")
  add_executable(nda-online-monitor devices/nda-online-monitor.cxx)
  target_link_libraries(nda-online-monitor PRIVATE nda_monitor FairMQ::FairMQ FairLogger::FairLogger)
  target_compile_options(nda-online-monitor PRIVATE ${NDA_WARNINGS})
  install(TARGETS nda-online-monitor RUNTIME DESTINATION bin)
endif()

# Original devices of NestdaqAnalyzer, kept until the new modules replace them.
if(NDA_BUILD_LEGACY)
  if(NOT EXISTS "${NESTDAQ_UNPACKER_SRC_DIR}/NodeUnpacker.cc")
    message(FATAL_ERROR "NDA_BUILD_LEGACY needs NESTDAQ_UNPACKER_SRC_DIR (NestdaqUnpacker/src)")
  endif()
  find_package(Boost REQUIRED COMPONENTS iostreams)
  include_directories(
    ${NESTDAQ_USER_IMPL_DIR}
    ${NESTDAQ_UNPACKER_SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/NestdaqTFSlicer
    ${Boost_INCLUDE_DIRS}
  )
  add_subdirectory(NestdaqTFSlicer)
  add_subdirectory(analyzer)
endif()

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
if(NDA_BUILD_TESTS)
  enable_testing()
  add_executable(test_format test/test_format.cc)
  target_link_libraries(test_format PRIVATE nda_format)
  add_test(NAME format COMMAND test_format)
  add_executable(test_modules test/test_modules.cc)
  target_link_libraries(test_modules PRIVATE nda_monitor)
  add_test(NAME modules COMMAND test_modules)
  set_target_properties(test_format test_modules PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
endif()
