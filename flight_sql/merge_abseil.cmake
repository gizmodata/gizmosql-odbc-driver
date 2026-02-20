# Merge all abseil static libraries into a single archive.
# This runs as an ExternalProject step after Arrow builds, since Arrow 23's
# bundled_dependencies archive does not include abseil symbols.
#
# Input variables (passed via -D):
#   ABSL_BUILD_DIR  — path to abseil build directory (_deps/absl-build)
#   OUTPUT_LIB      — output path for the merged archive
#   CMAKE_AR_TOOL   — path to ar (Linux only)

file(GLOB_RECURSE ABSL_LIBS "${ABSL_BUILD_DIR}/absl/*.a")
if(NOT ABSL_LIBS)
  message(FATAL_ERROR "No abseil libraries found in ${ABSL_BUILD_DIR}/absl/")
endif()

list(LENGTH ABSL_LIBS NUM_LIBS)
message(STATUS "Merging ${NUM_LIBS} abseil libraries into ${OUTPUT_LIB}")

if(APPLE)
  execute_process(
    COMMAND libtool -static -no_warning_for_no_symbols -o "${OUTPUT_LIB}" ${ABSL_LIBS}
    RESULT_VARIABLE RESULT
  )
else()
  # Linux: use ar with MRI script to merge archives
  set(MRI_CONTENT "CREATE ${OUTPUT_LIB}\n")
  foreach(lib ${ABSL_LIBS})
    string(APPEND MRI_CONTENT "ADDLIB ${lib}\n")
  endforeach()
  string(APPEND MRI_CONTENT "SAVE\nEND\n")

  get_filename_component(OUTPUT_DIR "${OUTPUT_LIB}" DIRECTORY)
  set(MRI_FILE "${OUTPUT_DIR}/merge_abseil.mri")
  file(WRITE "${MRI_FILE}" "${MRI_CONTENT}")

  execute_process(
    COMMAND "${CMAKE_AR_TOOL}" -M
    INPUT_FILE "${MRI_FILE}"
    RESULT_VARIABLE RESULT
  )
endif()

if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to merge abseil libraries (exit code: ${RESULT})")
endif()

message(STATUS "Successfully created ${OUTPUT_LIB}")
