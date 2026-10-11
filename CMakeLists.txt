if(DEFINED ZEPHYR_BASE)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/scripts/patch_pinnacle_driver.py"
            "${ZEPHYR_BASE}/drivers/input/input_pinnacle.c"
    RESULT_VARIABLE PINNACLE_PATCH_RESULT
    OUTPUT_VARIABLE PINNACLE_PATCH_OUTPUT
    ERROR_VARIABLE PINNACLE_PATCH_ERROR
  )

  if(NOT PINNACLE_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Could not apply Paljett's bounded Cirque startup recovery patch. "
      "${PINNACLE_PATCH_ERROR}${PINNACLE_PATCH_OUTPUT}"
    )
  endif()

  message(STATUS "${PINNACLE_PATCH_OUTPUT}")

  if(CONFIG_PALJETT_SENSOR_POWER_TEST)
    execute_process(
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_LIST_DIR}/scripts/patch_pinnacle_power_test.py"
              "${ZEPHYR_BASE}/drivers/input/input_pinnacle.c"
      RESULT_VARIABLE PALJETT_POWER_PATCH_RESULT
      OUTPUT_VARIABLE PALJETT_POWER_PATCH_OUTPUT
      ERROR_VARIABLE PALJETT_POWER_PATCH_ERROR
    )
    if(NOT PALJETT_POWER_PATCH_RESULT EQUAL 0)
      message(FATAL_ERROR
        "Could not apply Paljett sensor power test. "
        "${PALJETT_POWER_PATCH_ERROR}${PALJETT_POWER_PATCH_OUTPUT}"
      )
    endif()
    message(STATUS "${PALJETT_POWER_PATCH_OUTPUT}")
  endif()
endif()

if(CONFIG_PALJETT_ACCEL)
  zephyr_library()
  zephyr_library_sources(src/paljett_accel.c)
  zephyr_library_include_directories(${ZEPHYR_BASE}/../zmk/app/include)
endif()
