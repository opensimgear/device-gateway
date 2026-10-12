cmake_minimum_required(VERSION 3.24)
project(osg_device_gateway LANGUAGES C ASM)
option(OSG_BUILD_BOARD "Build STM32G0B1CB Rev A firmware" OFF)
add_library(osg_gateway_core STATIC src/gateway.c src/gateway_protocol.c src/gateway_registry.c src/gateway_journal.c)
target_include_directories(osg_gateway_core PUBLIC include)
target_compile_features(osg_gateway_core PUBLIC c_std_11)
target_compile_options(osg_gateway_core PRIVATE -Wall -Wextra -Werror)
if(NOT OSG_BUILD_BOARD)
  enable_testing()
  add_executable(osg_gateway_test tests/gateway_test.c)
  target_link_libraries(osg_gateway_test PRIVATE osg_gateway_core)
  target_compile_options(osg_gateway_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME gateway_behavior COMMAND osg_gateway_test)
  add_executable(osg_gateway_journal_test tests/journal_test.c)
  target_link_libraries(osg_gateway_journal_test PRIVATE osg_gateway_core)
  target_compile_options(osg_gateway_journal_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME gateway_flash_journal COMMAND osg_gateway_journal_test)
  return()
endif()
set(OSG_BOARD "gateway_g0b1cb_rev_a" CACHE STRING "Gateway board target")
if(NOT OSG_BOARD STREQUAL "gateway_g0b1cb_rev_a")
  message(FATAL_ERROR "Unknown OSG_BOARD: ${OSG_BOARD}")
endif()
set(OSG_USB_VID "" CACHE STRING "Authorized USB VID, decimal or 0x hex")
set(OSG_USB_PID "" CACHE STRING "Authorized USB PID, decimal or 0x hex")
foreach(id OSG_USB_VID OSG_USB_PID)
  if(NOT "${${id}}" MATCHES "^(0[xX][0-9a-fA-F]+|[0-9]+)$")
    message(FATAL_ERROR "Set ${id} to an identifier you are authorized to use; no public allocation exists yet")
  endif()
  math(EXPR id_value "${${id}}")
  if(id_value LESS 1 OR id_value GREATER 65535)
    message(FATAL_ERROR "${id} must be in 1..65535")
  endif()
endforeach()
include(cmake/dependencies.cmake)
set(HAL ${OSG_STM32_HAL_DIR})
set(CMSIS ${OSG_STM32_CMSIS_DIR})
add_library(osg_gateway_hal STATIC
  ${HAL}/Src/stm32g0xx_hal.c ${HAL}/Src/stm32g0xx_hal_cortex.c
  ${HAL}/Src/stm32g0xx_hal_rcc.c ${HAL}/Src/stm32g0xx_hal_rcc_ex.c
  ${HAL}/Src/stm32g0xx_hal_pwr.c ${HAL}/Src/stm32g0xx_hal_pwr_ex.c
  ${HAL}/Src/stm32g0xx_hal_flash.c ${HAL}/Src/stm32g0xx_hal_flash_ex.c
  ${HAL}/Src/stm32g0xx_hal_gpio.c ${HAL}/Src/stm32g0xx_hal_fdcan.c
  ${CMSIS}/Source/Templates/system_stm32g0xx.c)
target_include_directories(osg_gateway_hal PUBLIC include ${HAL}/Inc ${CMSIS}/Include ${OSG_CMSIS_CORE_DIR}/CMSIS/Core/Include)
target_compile_definitions(osg_gateway_hal PUBLIC STM32G0B1xx USE_HAL_DRIVER)
add_executable(osg_gateway src/main.c src/usb_descriptors.c
  boards/gateway_g0b1cb_rev_a/board.c boards/gateway_g0b1cb_rev_a/storage.c
  ${CMSIS}/Source/Templates/gcc/startup_stm32g0b1xx.s)
target_compile_definitions(osg_gateway PRIVATE OSG_USB_VID=${OSG_USB_VID} OSG_USB_PID=${OSG_USB_PID})
target_compile_options(osg_gateway PRIVATE -Wall -Wextra -Werror)
target_link_libraries(osg_gateway PRIVATE osg_gateway_core osg_core osg_usb osg_gateway_hal)
foreach(target osg_gateway osg_gateway_hal osg_gateway_core osg_core osg_usb osg_tinyusb osg_canard)
  target_compile_options(${target} PRIVATE -Os -ffunction-sections -fdata-sections -fstack-usage)
endforeach()
target_link_options(osg_gateway PRIVATE -T${CMAKE_CURRENT_SOURCE_DIR}/boards/gateway_g0b1cb_rev_a/flash.ld
  -nostartfiles --specs=nano.specs --specs=nosys.specs
  -Wl,--gc-sections,-Map=${CMAKE_CURRENT_BINARY_DIR}/osg_gateway.map -Wl,--print-memory-usage)
set_target_properties(osg_gateway PROPERTIES SUFFIX .elf)
add_custom_command(TARGET osg_gateway POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:osg_gateway> ${CMAKE_CURRENT_BINARY_DIR}/osg_gateway.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:osg_gateway> ${CMAKE_CURRENT_BINARY_DIR}/osg_gateway.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:osg_gateway>)
