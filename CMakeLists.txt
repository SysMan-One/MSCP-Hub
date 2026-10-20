#+++
#
#	FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
#
#	MODULE:		CMakeLists.txt
#
#	ABSTRACT:	Build of the MSCPHUB facility: the server, its help
#			libraries and its tests.
#
#	AUTHOR:		StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
#
#	CREATION DATE:	17-OCT-2026
#
#	MODIFICATION HISTORY:
#
#		20-OCT-2026	RRL	X01-10 : Initial public version.
#
#---

cmake_minimum_required(VERSION 3.16)

#
#	The identification of the product is the one of its head module: it is
#	read before PROJECT(), so that the version of the project is made of it
#	and cannot fall behind - X01-08 is 1.8.0
#
set(MH_HEADMOD "${CMAKE_CURRENT_SOURCE_DIR}/src/mscphub.c")

file(STRINGS "${MH_HEADMOD}" MH_IDENTLINE REGEX "^#define[ \t]+__IDENT__")

if ( NOT MH_IDENTLINE MATCHES "\"(X0*([0-9]+)-0*([0-9]+)(ECO0*([0-9]+))?)\"" )
	message(FATAL_ERROR "No __IDENT__ of the form Xnn-nn[ECOnn] in ${MH_HEADMOD}")
endif()

set(MH_IDENT "${CMAKE_MATCH_1}")

if ( "${CMAKE_MATCH_5}" STREQUAL "" )
	set(MH_VERSION "${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0")
else()
	set(MH_VERSION "${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.${CMAKE_MATCH_5}")
endif()

project(MSCPHUB VERSION ${MH_VERSION} LANGUAGES C DESCRIPTION "MSCP disk server for OpenVMS nodes, over NISCA")

#
#	The server is a package of its own: by default it is installed under
#	/opt/mscphub, and GNUInstallDirs, by the FHS, puts its configuration in
#	/etc/opt/mscphub.  -DCMAKE_INSTALL_PREFIX=... still chooses another place.
#	It must be set before GNUInstallDirs makes the full paths of it.
#
if ( CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT )
	set(CMAKE_INSTALL_PREFIX "/opt/mscphub" CACHE PATH "Where MSCPHUB is installed" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/mscphub")

#
#	__REV__ of every module must follow its __IDENT__: Xaa-bb[ECOcc] -> aa.bb.cc
#
function(s_mh_check_ident)
	foreach ( F ${ARGN} )
		file(STRINGS "${F}" L_IDENT REGEX "^#define[ \t]+__IDENT__")
		file(STRINGS "${F}" L_REV   REGEX "^#define[ \t]+__REV__")

		if ( NOT L_IDENT MATCHES "\"([^\"]+)\"" )
			message(FATAL_ERROR "No __IDENT__ in ${F}")
		endif()

		set(L_I "${CMAKE_MATCH_1}")

		if ( NOT L_REV MATCHES "\"([^\"]+)\"" )
			message(FATAL_ERROR "No __REV__ in ${F}")
		endif()

		set(L_R "${CMAKE_MATCH_1}")

		if ( NOT L_I MATCHES "^X0*([0-9]+)-0*([0-9]+)(ECO0*([0-9]+))?$" )
			message(FATAL_ERROR "${F}: __IDENT__ \"${L_I}\" is not of the form Xnn-nn[ECOnn]")
		endif()

		set(L_MAJ "${CMAKE_MATCH_1}")
		set(L_MIN "${CMAKE_MATCH_2}")
		set(L_ECO "${CMAKE_MATCH_4}")

		if ( L_ECO STREQUAL "" )
			set(L_ECO "0")
		endif()

		if ( NOT L_R STREQUAL "${L_MAJ}.${L_MIN}.${L_ECO}" )
			message(FATAL_ERROR "${F}: __REV__ \"${L_R}\" does not follow __IDENT__ \"${L_I}\" - it should be \"${L_MAJ}.${L_MIN}.${L_ECO}\"")
		endif()
	endforeach()
endfunction()

file(GLOB MH_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)

s_mh_check_ident(${MH_MODULES})

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MH_MODULES})

message(STATUS "MSCPHUB: ${MH_IDENT} (${PROJECT_VERSION}), from src/mscphub.c")

option(MH_STRICT "Treat the compilation diagnostics as errors" OFF)
option(BUILD_TESTING "Build the regression tests" ON)

add_library(mh_options INTERFACE)

target_compile_options(mh_options INTERFACE -Wall -Wextra -Wwrite-strings -Wno-unused-parameter)

if ( MH_STRICT )
	target_compile_options(mh_options INTERFACE -Werror)
endif()

find_package(StarLet 1.6 REQUIRED CONFIG)

message(STATUS "MSCPHUB: StarLet ${StarLet_VERSION} from ${StarLet_DIR}")

find_package(HELP 1.0 QUIET)

set(MH_HLBDIR "${CMAKE_INSTALL_DATAROOTDIR}/help" CACHE STRING "Directory the help libraries are installed into")

#
#	Everything but the head module is a library, so that the tests link
#	the very same objects the server does
#
add_library(mhcore STATIC
	src/mh_utl.c
	src/mh_lan.c
	src/mh_cc.c
	src/mh_tr.c
	src/mh_ppd.c
	src/mh_scs.c
	src/mh_blk.c
	src/mh_bk.c
	src/mh_mscp.c
	src/mh_hlp.c)

target_include_directories(mhcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

#	io_uring: the only engine of the backend (kernels of the last five years)
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)

target_link_libraries(mhcore PUBLIC StarLet::starlet PkgConfig::URING mh_options)

if ( HELP_FOUND )
	message(STATUS "MSCPHUB: /HELP answered out of the library, HELP ${HELP_VERSION} from ${HELP_DIR}")

	target_link_libraries(mhcore PUBLIC HELP::help)

	target_compile_definitions(mhcore PRIVATE
		MSCPHUB_WITH_HLB=1
		MSCPHUB_K_HLBSPEC="${CMAKE_INSTALL_PREFIX}/${MH_HLBDIR}/mscphub.hlb")

	help_add_library(NAME mscphublib ALL NOFACILITY
		SOURCE		${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphublib.md
		OUTPUT		${CMAKE_CURRENT_BINARY_DIR}/mscphub.hlb
		IDENT		${MH_IDENT}
		DESTINATION	${MH_HLBDIR})

	help_add_library(NAME mscphublib_ru ALL NOFACILITY
		SOURCE		${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphublib_ru.md
		OUTPUT		${CMAKE_CURRENT_BINARY_DIR}/mscphub_ru.hlb
		IDENT		${MH_IDENT}RU
		DESTINATION	${MH_HLBDIR}/ru)
else()
	message(STATUS "MSCPHUB: no HELP package found, /HELP answered by the summary built into the image")
endif()

add_executable(mscphub src/mscphub.c)

target_link_libraries(mscphub PRIVATE mhcore)

install(TARGETS mscphub RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

#
#	The service: the systemd unit, the sample configuration, the manual
#	page.  The unit and the page name the paths of this installation, so
#	they are made at configuration time.  The configuration itself,
#	mscphub.conf, is the administrator's: only the sample is installed.
#
option(MH_SYSTEMD "Install the systemd unit of the server (installed, never enabled)" ON)

set(MH_SYSTEMDDIR "/lib/systemd/system" CACHE PATH "Where the systemd unit is installed")

set(MH_CONFFILE		"${CMAKE_INSTALL_FULL_SYSCONFDIR}/mscphub.conf")
set(MH_FULL_SBINDIR	"${CMAKE_INSTALL_FULL_SBINDIR}")
set(MH_FULL_DOCDIR	"${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DOCDIR}")
set(MH_FULL_HLBDIR	"${CMAKE_INSTALL_PREFIX}/${MH_HLBDIR}")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphub.8.in ${CMAKE_CURRENT_BINARY_DIR}/mscphub.8 @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mscphub.8 DESTINATION ${CMAKE_INSTALL_MANDIR}/man8)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/etc/mscphub.conf.example DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR})

if ( MH_SYSTEMD )
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/etc/mscphub.service.in ${CMAKE_CURRENT_BINARY_DIR}/mscphub.service @ONLY)

	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mscphub.service DESTINATION ${MH_SYSTEMDDIR})

	#
	#	systemd is told of the new unit; it is neither enabled nor
	#	started - a server that starts by itself begins to talk to the
	#	cluster on its LAN.  Not for a staged installation (DESTDIR), a
	#	host without systemctl or an unprivileged user.
	#
	install(CODE "
		if ( \"\$ENV{DESTDIR}\" STREQUAL \"\" AND EXISTS /run/systemd/system )
			execute_process(COMMAND systemctl daemon-reload RESULT_VARIABLE L_RC)
			if ( NOT L_RC EQUAL 0 )
				message(WARNING \"systemctl daemon-reload failed (\${L_RC}); run it by hand\")
			endif()
		endif()")
endif()

install(FILES
	${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
	${CMAKE_CURRENT_SOURCE_DIR}/README.md
	${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphub_guide.md
	${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphub_guide_ru.md
	${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphublib.md
	${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphublib_ru.md
	DESTINATION ${CMAKE_INSTALL_DOCDIR})

#
#	'uninstall' - it removes what the install step of this build directory
#	recorded in install_manifest.txt, and nothing else
#
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake @ONLY)

add_custom_target(uninstall
	COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake
	COMMENT "Removing the files listed in install_manifest.txt")

#
#	The kit: MSCPHUB-<ident>.tar.gz, the directory within it named the
#	same.  The sources, the documentation and the tests, as git keeps
#	them, and nothing of a build tree.
#
set(MH_KIT "MSCPHUB-${MH_IDENT}")

add_custom_target(dist
	COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/kit
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/kit/${MH_KIT}
	COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} archive --format=tar HEAD | ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}/kit/${MH_KIT} tar xf -
	COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}/kit ${CMAKE_COMMAND} -E tar czf ${CMAKE_CURRENT_BINARY_DIR}/${MH_KIT}.tar.gz ${MH_KIT}
	COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/kit
	COMMENT "Making ${MH_KIT}.tar.gz of the committed sources (git archive HEAD)"
	VERBATIM)

if ( BUILD_TESTING )
	enable_testing()
	add_subdirectory(tests)
endif()
