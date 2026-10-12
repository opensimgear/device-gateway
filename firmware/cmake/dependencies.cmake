# Keep fetched source clones under .research, as required by workspace policy.
# This must precede include(FetchContent), which installs a build/_deps default.
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
  set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.research/gateway-deps" CACHE PATH "Dependency clones")
endif()
include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/common-revision.cmake)
FetchContent_Declare(firmware_common GIT_REPOSITORY https://github.com/opensimgear/firmware-common.git
  GIT_TAG ${OSG_COMMON_REVISION} GIT_SUBMODULES "")
set(OSG_EMBEDDED ON CACHE BOOL "" FORCE)
set(OSG_USB_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
FetchContent_MakeAvailable(firmware_common)
