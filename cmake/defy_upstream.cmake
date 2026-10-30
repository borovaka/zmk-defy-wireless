# SPDX-License-Identifier: MIT
#
# Checks on the upstream code the module relies on beyond ZMK's and Zephyr's
# APIs; README.md, "Changes to ZMK and Zephyr", lists each use. A failed check
# stops the build until the use named in its message is reviewed against the
# new upstream code.

# Stops unless file has the SHA-256 sha256: for code the module patches or
# hooks into by its exact contents.
function(defy_upstream_pin file sha256 use)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${file})
  file(SHA256 ${file} actual)
  if(NOT actual STREQUAL sha256)
    message(FATAL_ERROR "${file} changed (SHA-256 ${actual}); review ${use} against it")
  endif()
endfunction()

# Stops unless file still holds each text after use, such as the call that a
# link-time wrap or a rename relies on: a wrap of a function no one calls any
# more would link and never run.
function(defy_upstream_uses file use)
  if(NOT EXISTS ${file})
    message(FATAL_ERROR "${file} is gone; review ${use} against the new upstream code")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${file})
  file(READ ${file} text)
  foreach(expected IN LISTS ARGN)
    string(FIND "${text}" "${expected}" found)
    if(found EQUAL -1)
      message(FATAL_ERROR "${file} no longer has \"${expected}\"; review ${use} against it")
    endif()
  endforeach()
endfunction()
