# FindVART.cmake
#
# Locate the Xilinx VART (Vitis AI Runtime) and XIR headers/libraries.
#
# This module defines:
#   VART_FOUND          - TRUE if VART and XIR are found
#   VART_INCLUDE_DIRS   - Include directories for vart/ and xir/
#   VART_LIBRARIES      - Libraries to link against
#   VART_VERSION        - Detected VART version string
#
# Imported targets:
#   VART::VART          - INTERFACE target with includes and link libraries
#
# Hints:
#   VART_ROOT           - Prefix where VART/XIR are installed
#
# The module searches standard Xilinx install locations such as /usr/include
# and /usr/lib, which is where the Vitis AI 1.4.1 Docker image installs them.

if (VART_ROOT)
  list(APPEND _vart_search_paths "${VART_ROOT}/include" "${VART_ROOT}/lib")
endif ()

find_path(VART_INCLUDE_DIR
  NAMES vart/runner.hpp
  PATHS ${_vart_search_paths}
        /usr/include
        /opt/vitis_ai/compiler/include
)

find_path(XIR_INCLUDE_DIR
  NAMES xir/graph/graph.hpp
  PATHS ${_vart_search_paths}
        /usr/include
        /opt/vitis_ai/compiler/include
)

find_library(VART_RUNNER_LIBRARY
  NAMES vart-runner
  PATHS ${_vart_search_paths}
        /usr/lib /usr/lib/x86_64-linux-gnu /opt/vitis_ai/compiler/lib
)

find_library(XIR_LIBRARY
  NAMES xir
  PATHS ${_vart_search_paths}
        /usr/lib /usr/lib/x86_64-linux-gnu /opt/vitis_ai/compiler/lib
)

# Tensor-buffer helper utilities (get_tensor_buffer_data, alloc_cpu_flat_tensor_buffer, ...)
find_library(VART_RUNNER_HELPER_LIBRARY
  NAMES vitis_ai_library-runner_helper
  PATHS ${_vart_search_paths}
        /usr/lib /usr/lib/x86_64-linux-gnu /opt/vitis_ai/compiler/lib
)

# Optional but useful at link time for some VART utilities.
find_library(VART_BUFFER_OBJECT_LIBRARY
  NAMES vart-buffer-object
  PATHS ${_vart_search_paths}
        /usr/lib /usr/lib/x86_64-linux-gnu /opt/vitis_ai/compiler/lib
)

find_library(VART_MEM_MANAGER_LIBRARY
  NAMES vart-mem-manager
  PATHS ${_vart_search_paths}
        /usr/lib /usr/lib/x86_64-linux-gnu /opt/vitis_ai/compiler/lib
)

find_library(VART_UTIL_LIBRARY
  NAMES vart-util
  PATHS ${_vart_search_paths}
        /usr/lib /usr/lib/x86_64-linux-gnu /opt/vitis_ai/compiler/lib
)

# Try to infer the VART version from the versioned soname of libvart-runner.so.
set(VART_VERSION "unknown")
if (VART_RUNNER_LIBRARY)
  get_filename_component(_vart_runner_dir "${VART_RUNNER_LIBRARY}" DIRECTORY)
  get_filename_component(_vart_runner_name "${VART_RUNNER_LIBRARY}" NAME_WE)
  file(GLOB _vart_runner_versioned "${_vart_runner_dir}/${_vart_runner_name}.so.*")
  if (_vart_runner_versioned)
    list(GET _vart_runner_versioned 0 _vart_runner_first)
    get_filename_component(_vart_runner_soname "${_vart_runner_first}" NAME)
    string(REGEX REPLACE "^${_vart_runner_name}\\.so\\.([0-9.]+).*" "\\1" VART_VERSION "${_vart_runner_soname}")
  endif ()
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(VART
  REQUIRED_VARS
    VART_INCLUDE_DIR
    XIR_INCLUDE_DIR
    VART_RUNNER_LIBRARY
    XIR_LIBRARY
    VART_RUNNER_HELPER_LIBRARY
  VERSION_VAR VART_VERSION
)

if (VART_FOUND)
  set(VART_INCLUDE_DIRS
    ${VART_INCLUDE_DIR}
    ${XIR_INCLUDE_DIR}
  )
  set(VART_LIBRARIES
    ${VART_RUNNER_LIBRARY}
    ${VART_RUNNER_HELPER_LIBRARY}
    ${XIR_LIBRARY}
  )
  foreach (_lib VART_BUFFER_OBJECT_LIBRARY VART_MEM_MANAGER_LIBRARY VART_UTIL_LIBRARY)
    if (${_lib})
      list(APPEND VART_LIBRARIES ${${_lib}})
    endif()
  endforeach()

  if (NOT TARGET VART::VART)
    add_library(VART::VART INTERFACE IMPORTED)
    set_target_properties(VART::VART PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${VART_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "${VART_LIBRARIES}"
    )
  endif()
endif()

mark_as_advanced(
  VART_INCLUDE_DIR
  XIR_INCLUDE_DIR
  VART_RUNNER_LIBRARY
  XIR_LIBRARY
  VART_RUNNER_HELPER_LIBRARY
  VART_BUFFER_OBJECT_LIBRARY
  VART_MEM_MANAGER_LIBRARY
  VART_UTIL_LIBRARY
)
