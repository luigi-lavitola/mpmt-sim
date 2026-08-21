cmake_minimum_required(VERSION 3.16)
# C is needed as well as C++: ToolDAQFramework's ServiceDiscovery compiles
# zstd.c, and the mss client's own project() call resets language setup in a
# way that leaves CMAKE_C_COMPILE_OBJECT unset if C was never enabled here.
project(mpmt-sim VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# The two sibling checkouts this builds against. Sources are compiled from
# them directly rather than copied, so sim-producer speaks whatever protocol
# mpmt-daq-interface currently speaks — there is no second copy to drift.
#
# mpmt-daq-interface (this variable's old default) is superseded by
# m-pmt-daq-interface (built on ElectronicsTemplate, the DAQ group's base -
# see that repo's docs/migration-from-mpmt-daq-interface.md) as of
# 2026-08-14. Kept the same variable name for anyone with a build script
# already pointing MPMT_DAQ_INTERFACE_DIR at a local override.
set(MPMT_DAQ_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../m-pmt-daq-interface"
    CACHE PATH "path to the m-pmt-daq-interface checkout")
set(TOOLDAQ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libToolDAQ"
    CACHE PATH "path to the libToolDAQ checkout")

if(NOT EXISTS "${MPMT_DAQ_INTERFACE_DIR}/src/DataSender.cpp")
  message(FATAL_ERROR "m-pmt-daq-interface not found at ${MPMT_DAQ_INTERFACE_DIR}")
endif()

set(MPMT_MSS_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mpmt-mss/src/mpmt_mss/client/cpp"
    CACHE PATH "path to the mpmt-mss C++ client")

add_subdirectory(${TOOLDAQ_DIR} ${CMAKE_BINARY_DIR}/libToolDAQ)

# The mss client pulls in libcurl and nlohmann_json. Building it is what lets
# sim-producer expose the mss slow controls and monitoring exactly as the real
# producer does, instead of leaving the simulated mss reachable only over HTTP.
set(MSSCLIENT_BUILD_EXAMPLES OFF CACHE BOOL "Build mssclient example programs" FORCE)
add_subdirectory(${MPMT_MSS_CPP_DIR} ${CMAKE_BINARY_DIR}/mssclient)

find_package(cppzmq REQUIRED)

# --- payload generator: no dependencies, so it can be unit tested alone ----

add_library(mpmt_payload STATIC src/payload/mpmt_payload.cpp)
target_include_directories(mpmt_payload PUBLIC src/payload)

add_executable(test-payload tests/test_payload.cpp)
target_link_libraries(test-payload PRIVATE mpmt_payload)

enable_testing()
add_test(NAME payload COMMAND test-payload)

# --- simulated producer ---------------------------------------------------
#
# Everything but producer.cpp itself is compiled from mpmt-daq-interface: the
# mss integration (MSSIntegration, MSSSlowControl) and DataSender (kept local
# there for its disk-cache-on-congestion addition) are the production
# sources, so a simulated node exposes the same slow controls and the same
# monitoring payload as a real one. DAQInterface/DAQHeader/DataMessages are
# NOT compiled from there any more (verified byte-identical to upstream
# modulo line endings, 2026-08-14) - ToolDAQ below provides them. Only the
# event source differs from the real producer - a PayloadGenerator instead
# of the DMA-proxy device.

add_executable(sim-producer
  src/producer/sim_producer.cpp
  ${MPMT_DAQ_INTERFACE_DIR}/src/DataSender.cpp
  ${MPMT_DAQ_INTERFACE_DIR}/src/MSSIntegration.cpp
  ${MPMT_DAQ_INTERFACE_DIR}/src/MSSSlowControl.cpp
)

# ${CMAKE_BINARY_DIR}/include is where libToolDAQ gathers the framework
# headers (SlowControlCollection.h and friends) — m-pmt-daq-interface's own
# targets include it for the same reason. DAQInterface_static's own include
# dir (DAQInterface.h/DataSender.h/...) doesn't come along with linking
# ToolDAQ below - that merge is PRIVATE - so it's pulled in directly here,
# same fix as m-pmt-daq-interface/CMakeLists.txt.
target_include_directories(sim-producer PRIVATE
  ${MPMT_DAQ_INTERFACE_DIR}/include
  ${CMAKE_BINARY_DIR}/include
  $<TARGET_PROPERTY:DAQInterface_static,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(sim-producer PRIVATE mpmt_payload ToolDAQ mssclient cppzmq stdc++fs pthread)
