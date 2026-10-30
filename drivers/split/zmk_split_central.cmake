# SPDX-License-Identifier: MIT

# Writes ZMK's split/bluetooth/central.c with zmk-split-central.patch applied
# to <output>. The patch names the upstream file's SHA-256; any other version
# stops the build until the patch is reviewed.

find_package(Git REQUIRED)

set(DEFY_SPLIT_CENTRAL_PATCH ${CMAKE_CURRENT_LIST_DIR}/zmk-split-central.patch)

function(defy_split_central_source source output)
  set(patch ${DEFY_SPLIT_CENTRAL_PATCH})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${patch})

  file(STRINGS ${patch} pinned REGEX "^SHA-256: " LIMIT_COUNT 1)
  string(REPLACE "SHA-256: " "" pinned "${pinned}")
  defy_upstream_pin(${source} ${pinned} ${patch})

  get_filename_component(directory ${output} DIRECTORY)
  set(work ${directory}/split-central-patch)
  file(REMOVE_RECURSE ${work})
  file(MAKE_DIRECTORY ${work})
  file(COPY_FILE ${source} ${work}/central.c)
  # The ceiling keeps git from treating an enclosing repository as its own,
  # so git apply works on the copy as patch(1) would, without fuzz.
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env GIT_CEILING_DIRECTORIES=${directory}
            ${GIT_EXECUTABLE} apply ${patch}
    WORKING_DIRECTORY ${work}
    RESULT_VARIABLE result
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot apply ${patch}: ${error}")
  endif()
  # Only a changed result touches the output, so unchanged builds stay cached.
  file(COPY_FILE ${work}/central.c ${output} ONLY_IF_DIFFERENT)
endfunction()
