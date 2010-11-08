ADD_EXECUTABLE(solv solv.c ../common/myfopen.c)
IF ( DEBIAN )
TARGET_LINK_LIBRARIES(solv satsolverext satsolver ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})
ELSE ( DEBIAN )
TARGET_LINK_LIBRARIES(solv satsolverext satsolver ${RPMDB_LIBRARY} ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})
ENDIF ( DEBIAN )

install(TARGETS
    solv
    DESTINATION ${BIN_INSTALL_DIR})
