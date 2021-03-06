#
# CMakeLists.txt for bindings/ruby
#

ENABLE_TESTING()
ADD_SUBDIRECTORY(tests)

# SWIG_OUPUT is per-target

SET( SWIG_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/satsolver_ruby.c" )

MESSAGE(STATUS "Ruby executable: ${RUBY_EXECUTABLE}")
MESSAGE(STATUS "Ruby vendor arch dir: ${RUBY_VENDORARCH_DIR}")
MESSAGE(STATUS "Ruby include path: ${RUBY_INCLUDE_PATH}")

ADD_CUSTOM_COMMAND (
   OUTPUT  "${SWIG_OUTPUT}"
   COMMAND ${CMAKE_COMMAND} -E echo_append "Creating wrapper code for Ruby..."
   COMMAND ${SWIG_EXECUTABLE} -ruby -o ${SWIG_OUTPUT} ${SWIG_INPUT}
   COMMAND ${CMAKE_COMMAND} -E echo "Done."
   WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../*.i
)

SET(bindings_SRCS ${SWIG_OUTPUT} "${CMAKE_SOURCE_DIR}/ext/repo_rpmdb.c" )

ADD_LIBRARY( satsolver_ruby SHARED ${bindings_SRCS} )

# name it '_satsolver.so'
#  and don't prefix with 'lib'
SET_TARGET_PROPERTIES( satsolver_ruby PROPERTIES OUTPUT_NAME "satsolver" PREFIX "" )

INCLUDE_DIRECTORIES( ${RUBY_INCLUDE_PATH} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/applayer ${CMAKE_SOURCE_DIR}/ext )

TARGET_LINK_LIBRARIES( satsolver_ruby satsolver )
TARGET_LINK_LIBRARIES( satsolver_ruby appsatsolver )
TARGET_LINK_LIBRARIES( satsolver_ruby ${RUBY_LIBRARY} )
TARGET_LINK_LIBRARIES( satsolver_ruby ${RPMDB_LIBRARY} )

INSTALL(TARGETS satsolver_ruby LIBRARY DESTINATION ${RUBY_VENDORARCH_DIR})

# copy satsolver.rb to LIB_DIR
INSTALL(FILES ${CMAKE_CURRENT_SOURCE_DIR}/satsolver.rb DESTINATION ${RUBY_VENDORLIB_DIR})
# copy satsolver/*.rb to LIB_DIR
INSTALL(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/satsolver DESTINATION ${RUBY_VENDORLIB_DIR}
   PATTERN "*.rb"
   PATTERN "*~" EXCLUDE)
		
# rdoc
SET(rdoc_dir "${CMAKE_CURRENT_BINARY_DIR}/html")
ADD_CUSTOM_COMMAND (
   OUTPUT ${rdoc_dir}
   COMMAND ${CMAKE_COMMAND} -E echo_append "Creating rdoc documentation ..."
   COMMAND rm -rf ${rdoc_dir}
   COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/rdoc -o ${rdoc_dir} satsolver.i *.i
   COMMAND ${CMAKE_COMMAND} -E echo "Done."
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/bindings
   DEPENDS ${CMAKE_SOURCE_DIR}/bindings/*.i
)
ADD_CUSTOM_TARGET(ruby_rdoc ALL DEPENDS "${rdoc_dir}")
ADD_DEPENDENCIES(ruby_rdoc satsolver_ruby)

#
# Leave this to %doc in the .spec file
#INSTALL(DIRECTORY "${rdoc_dir}" DESTINATION ${DOC_INSTALL_DIR})
