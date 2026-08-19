cmake_minimum_required(VERSION 3.10)

project(
  esp32_bluetooth_classic_sniffer
  LANGUAGES CXX C
  VERSION 1.0)

set(PYTHON3_URL
    "https://github.com/indygreg/python-build-standalone/releases/download/20210506/cpython-3.8.10-x86_64-unknown-linux-gnu-pgo+lto-20210506T0943.tar.zst"
)

set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
# libbtstack.so and the programs share one output dir (build/host_stack), so $ORIGIN
# lets each binary find the library next to it.
set(CMAKE_BUILD_RPATH $ORIGIN runtime/install/lib/)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/build/host_stack)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/build/host_stack)

# * pthreads
find_package(Threads REQUIRED)

if(NOT EXISTS ${PROJECT_SOURCE_DIR}/runtime/success)
  file(DOWNLOAD ${PYTHON3_URL} ${PROJECT_SOURCE_DIR}/runtime.tar.zst
       SHOW_PROGRESS)
  execute_process(
    COMMAND
      bash -c
      "tar -I zstd -xf runtime.tar.zst && rm runtime.tar.zst && cp -R python/* runtime && rm -rd python"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ERROR_VARIABLE PYTHON_ERROR)
  if(PYTHON_ERROR)
    message(FATAL_ERROR ${PYTHON_ERROR})
  endif(PYTHON_ERROR)
  # Install requirements from requirements.txt
  execute_process(
    COMMAND bash -c
            "./runtime/install/bin/python3 -m pip install -r ./requirements.txt"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ERROR_VARIABLE PYTHON_ERROR)
  if(PYTHON_ERROR)
    message(FATAL_ERROR ${PYTHON_ERROR})
  endif(PYTHON_ERROR)
  # Save success indication file
  execute_process(
    COMMAND bash -c "touch runtime/success"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ERROR_VARIABLE PYTHON_ERROR)
  if(PYTHON_ERROR)
    message(FATAL_ERROR ${PYTHON_ERROR})
  endif(PYTHON_ERROR)
endif()
set(PYTHON_EXECUTABLE ${PROJECT_SOURCE_DIR}/runtime/install/bin/python3)

# Bluetooth host stack (libbtstack.so + sdp_rfcomm_query/spp_counter).
# Everything is defined in host_stack/CMakeLists.txt: it builds the shared library from the
# pristine clone (host_stack/btstack) with host_stack/overlay overriding config + a few
# patched files, and links the host programs (host_stack/programs) against it.
add_subdirectory(${PROJECT_SOURCE_DIR}/host_stack)

message("Build RPath: ${CMAKE_BUILD_RPATH}")
