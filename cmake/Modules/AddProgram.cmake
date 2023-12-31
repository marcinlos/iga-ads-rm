function(add_program name)
  cmake_parse_arguments(PROGRAM "MUMPS;GALOIS" "" "LIBS;SRC" ${ARGN})

  set(OK TRUE)
  if (PROGRAM_GALOIS AND NOT ADS_USE_GALOIS)
    set(OK FALSE)
  endif()
  if (PROGRAM_MUMPS AND NOT ADS_USE_MUMPS)
    set(OK FALSE)
  endif()

  if (OK)
    add_executable(${name} ${PROGRAM_SRC})
    target_link_libraries(${name} PRIVATE ADS ads-options-private ${PROGRAM_LIBS})
  endif()
endfunction()
