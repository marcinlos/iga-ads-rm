if (NOT ADS_BUILD_PROBLEMS)
  return()
endif()

include(AddProgram)

# Custom target that depends on all example applications
add_custom_target(ads-examples)

# Helper function prefixing target names with "ads-example-"
function(add_example name)
  set(TARGET_NAME "ads-example-${name}")
  add_program(${TARGET_NAME} ${ARGN})

  if (TARGET ${TARGET_NAME})
    set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME ${name})
    add_dependencies(ads-examples ${TARGET_NAME})
  endif()
endfunction()

find_package(lyra REQUIRED)

# --------------------------------------------------------------------
# Problem definitions
# --------------------------------------------------------------------

add_example(heat_1d
  SRC
  heat/heat_1d.cpp)

add_example(heat_2d GALOIS
  SRC
  heat/heat_2d.cpp)

add_example(heat_3d
  SRC
  heat/heat_3d.cpp)

add_example(erikkson_nonstationary MUMPS GALOIS
  SRC
  erikkson/main_nonstationary.cpp)

add_example(stokes_projection MUMPS GALOIS
  SRC
  stokes/main_projection.cpp)

add_example(cg MUMPS GALOIS
  SRC
    cg/main.cpp
    cg/shishkin.cpp
  LIBS
    bfg::lyra
)

add_example(dg_laplace MUMPS GALOIS
  SRC
  dg/laplace.cpp)
