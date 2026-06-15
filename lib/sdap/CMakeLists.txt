#
# Copyright 2021-2026 Software Radio Systems Limited
#
# This file is part of srsRAN
#
# srsRAN is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# srsRAN is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# A copy of the GNU Affero General Public License can be found in
# the LICENSE file in the top-level directory of this distribution
# and at http://www.gnu.org/licenses/.
#

set(SOURCES
    sdap_factory.cpp
    sdap_tx_fpga_notifier.cpp)

if (ENABLE_FPGA_VART)
    list(APPEND SOURCES sdap_tx_vart_notifier.cpp)
endif ()

add_library(srsran_sdap STATIC ${SOURCES})
target_link_libraries(srsran_sdap)

if (ENABLE_FPGA_VART)
    target_link_libraries(srsran_sdap VART::VART ${OpenCV_LIBS})
    target_include_directories(srsran_sdap SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    # VART/XIR headers use typeid; re-enable RTTI for the VART backend source.
    set_source_files_properties(sdap_tx_vart_notifier.cpp PROPERTIES COMPILE_FLAGS "-frtti")
    # Expose the feature flag to source files so the factory can conditionally
    # reference the VART offload notifier.
    target_compile_definitions(srsran_sdap PUBLIC ENABLE_FPGA_VART)
endif ()
