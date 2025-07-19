# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/dp/esp/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "/Users/dp/esp/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "/Users/dp/Projects/esp/hfp_ag_custom/build/bootloader"
  "/Users/dp/Projects/esp/hfp_ag_custom/build/bootloader-prefix"
  "/Users/dp/Projects/esp/hfp_ag_custom/build/bootloader-prefix/tmp"
  "/Users/dp/Projects/esp/hfp_ag_custom/build/bootloader-prefix/src/bootloader-stamp"
  "/Users/dp/Projects/esp/hfp_ag_custom/build/bootloader-prefix/src"
  "/Users/dp/Projects/esp/hfp_ag_custom/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/dp/Projects/esp/hfp_ag_custom/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/dp/Projects/esp/hfp_ag_custom/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
