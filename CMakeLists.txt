cmake_minimum_required(VERSION 3.9)

project(libgdbstub VERSION 0.0.0 DESCRIPTION "Freestanding GDB stub library")

add_library(gdbstub SHARED
    gdb-stub.c
)

set_target_properties(gdbstub PROPERTIES VERSION ${PROJECT_VERSION})
set_target_properties(gdbstub PROPERTIES SOVERSION 0)
set_target_properties(gdbstub PROPERTIES PUBLIC_HEADER libgdbstub.h)
target_include_directories(gdbstub PRIVATE .)

add_library(gdbstubstatic STATIC
    gdb-stub.c
)
set_target_properties(gdbstubstatic PROPERTIES OUTPUT_NAME gdbstub)
set_target_properties(gdbstubstatic PROPERTIES PUBLIC_HEADER libgdbstub.h)
target_include_directories(gdbstubstatic PRIVATE .)

include(GNUInstallDirs)
install(TARGETS gdbstub
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS gdbstubstatic
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_file(libgdbstub.pc.in libgdbstub.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/libgdbstub.pc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pkgconfig)
