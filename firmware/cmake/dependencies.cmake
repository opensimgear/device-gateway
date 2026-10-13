# Dependencies belong to this build, so a standalone checkout is sufficient.
include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/common-revision.cmake)
FetchContent_Declare(firmware_common GIT_REPOSITORY https://github.com/opensimgear/firmware-common.git
  GIT_TAG ${OSG_COMMON_REVISION} GIT_SUBMODULES "")
set(OSG_EMBEDDED ON CACHE BOOL "" FORCE)
set(OSG_USB_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
FetchContent_MakeAvailable(firmware_common)
