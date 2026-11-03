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
#		22-OCT-2026	RRL	X01-16 : the manuals of the DEC layout (User's Guide,
#						Programmer's Guide), MH_PDF renders them.
#		22-OCT-2026	RRL	X01-17 : MHMON and the collector of T4; Modbus/TCP needs the
#						threads.
#		22-OCT-2026	RRL	X01-17 : the units of systemd only where systemd runs.
#		22-OCT-2026	RRL	X01-18 : the scripts of OpenRC and System V init where systemd
#						does not run (MH_INIT).
#		22-OCT-2026	RRL	X01-19 : the parameter file in /etc/mscphub (MH_CONFDIR), not
#						/etc/opt/mscphub.
#		25-OCT-2026	RRL	X01-22 : MHODS, the reader of Files-11, and MHMOP, the MOP server.
#		03-NOV-2026	RRL	X01-24 : librt for shm_open on glibc older than 2.34.
#		03-NOV-2026	RRL	X01-24ECO1 : the .deb by
#					CPack (cpack -G DEB), built by .github/workflows/release.yml.
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
#	/opt/mscphub, its configuration is in /etc/mscphub (MH_CONFDIR, below).
#	-DCMAKE_INSTALL_PREFIX=... still chooses another place.  It must be set
#	before GNUInstallDirs makes the full paths of it.
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
	src/mh_ods.c
	src/mh_mop.c
	src/mh_stat.c
	src/mh_mbus.c
	src/mh_hlp.c)

target_include_directories(mhcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

#	io_uring: the only engine of the backend (kernels of the last five years)
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)

#	The counters over Modbus/TCP are served by a thread of their own
find_package(Threads REQUIRED)

#	shm_open()/shm_unlink() are in librt before glibc 2.34 (Ubuntu 20.04, Debian 10)
find_library(MH_RT rt)

target_link_libraries(mhcore PUBLIC StarLet::starlet PkgConfig::URING Threads::Threads mh_options ${MH_RT})

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

#
#	MHMON, the monitor of the server: it reads the statistics block
#
add_executable(mhmon src/mhmon.c)

target_link_libraries(mhmon PRIVATE mhcore)

install(TARGETS mscphub mhmon RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

#
#	The service: the systemd unit, the sample configuration, the manual
#	page.  The unit and the page name the paths of this installation, so
#	they are made at configuration time.  The configuration itself,
#	mscphub.conf, is the administrator's: only the sample is installed.
#
#
#	The units of systemd are installed where systemd runs; elsewhere the
#	server is started by the init of the host (-DMH_SYSTEMD=ON forces them)
#
if ( EXISTS /run/systemd/system )
	set(MH_K_SYSTEMD ON)
else()
	set(MH_K_SYSTEMD OFF)
endif()

option(MH_SYSTEMD "Install the systemd units of the server (installed, never enabled)" ${MH_K_SYSTEMD})

set(MH_SYSTEMDDIR "/lib/systemd/system" CACHE PATH "Where the systemd unit is installed")

#
#	The parameter file: in /etc/mscphub, where the administrator looks for
#	it and the backup of /etc takes it, for the default prefix and for /usr;
#	in etc/mscphub of any other prefix - /usr/local/etc/mscphub
#
if ( CMAKE_INSTALL_PREFIX STREQUAL "/opt/mscphub" OR CMAKE_INSTALL_PREFIX STREQUAL "/usr" )
	set(MH_K_CONFDIR "/etc/mscphub")
else()
	set(MH_K_CONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/mscphub")
endif()

set(MH_CONFDIR		"${MH_K_CONFDIR}" CACHE PATH "Where the parameter file of the service is")
set(MH_CONFFILE		"${MH_CONFDIR}/mscphub.conf")
set(MH_FULL_SBINDIR	"${CMAKE_INSTALL_FULL_SBINDIR}")
set(MH_FULL_DOCDIR	"${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DOCDIR}")
set(MH_FULL_HLBDIR	"${CMAKE_INSTALL_PREFIX}/${MH_HLBDIR}")
set(MH_T4DIR		"/var/opt/mscphub/t4" CACHE PATH "Where the collector of T4 keeps its files")
set(MH_INITDIR		"/etc/init.d" CACHE PATH "Where the init scripts are installed, without systemd")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphub.8.in ${CMAKE_CURRENT_BINARY_DIR}/mscphub.8 @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mscphub.8 DESTINATION ${CMAKE_INSTALL_MANDIR}/man8)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/etc/mscphub.conf.example DESTINATION ${MH_CONFDIR})

#
#	Before X01-19 the parameter file was in /etc/opt/mscphub.  It is the
#	administrator's and is not moved; the installation says where it goes.
#
install(CODE "
	if ( \"\$ENV{DESTDIR}\" STREQUAL \"\" AND EXISTS /etc/opt/mscphub/mscphub.conf AND NOT EXISTS ${MH_CONFFILE} )
		message(WARNING \"/etc/opt/mscphub/mscphub.conf is no longer read: move it to ${MH_CONFFILE}\")
	endif()")

if ( MH_SYSTEMD )
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/etc/mscphub.service.in ${CMAKE_CURRENT_BINARY_DIR}/mscphub.service @ONLY)

	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mscphub.service DESTINATION ${MH_SYSTEMDDIR})

	#
	#	The collector of T4: MHMON records the counters every minute, a
	#	file a day.  Installed, never enabled.
	#
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/etc/mscphub-t4.service.in ${CMAKE_CURRENT_BINARY_DIR}/mscphub-t4.service @ONLY)

	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mscphub-t4.service DESTINATION ${MH_SYSTEMDDIR})

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

#
#	Without systemd the scripts of the init of the host: OpenRC where
#	openrc-run is (Alpine, Gentoo, Artix), System V init where there is
#	/etc/init.d (Devuan, Slackware), none elsewhere.  Both kinds are made
#	always - the tests run the System V ones - and one of them installed,
#	never added to a runlevel.  -DMH_INIT=OPENRC|SYSV|NONE chooses.
#
if ( MH_SYSTEMD )
	set(MH_K_INIT NONE)
elseif ( EXISTS /sbin/openrc-run OR EXISTS /usr/sbin/openrc-run )
	set(MH_K_INIT OPENRC)
elseif ( IS_DIRECTORY /etc/init.d )
	set(MH_K_INIT SYSV)
else()
	set(MH_K_INIT NONE)
endif()

set(MH_INIT "${MH_K_INIT}" CACHE STRING "The init scripts of the server: OPENRC, SYSV or NONE (installed, never enabled)")
set_property(CACHE MH_INIT PROPERTY STRINGS OPENRC SYSV NONE)

if ( NOT MH_INIT MATCHES "^(OPENRC|SYSV|NONE)$" )
	message(FATAL_ERROR "MH_INIT is ${MH_INIT}, not OPENRC, SYSV or NONE")
endif()

foreach ( MH_KIND openrc sysv )
	foreach ( MH_SCRIPT mscphub mscphub-t4 )
		configure_file(${CMAKE_CURRENT_SOURCE_DIR}/etc/${MH_SCRIPT}.${MH_KIND}.in
			${CMAKE_CURRENT_BINARY_DIR}/${MH_KIND}/${MH_SCRIPT} @ONLY)
	endforeach()
endforeach()

if ( NOT MH_INIT STREQUAL "NONE" )
	string(TOLOWER ${MH_INIT} MH_KIND)

	install(PROGRAMS
		${CMAKE_CURRENT_BINARY_DIR}/${MH_KIND}/mscphub
		${CMAKE_CURRENT_BINARY_DIR}/${MH_KIND}/mscphub-t4
		DESTINATION ${MH_INITDIR})
endif()

message(STATUS "MSCPHUB: systemd units ${MH_SYSTEMD}, init scripts ${MH_INIT}")

install(FILES
	${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
	${CMAKE_CURRENT_SOURCE_DIR}/README.md
	${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphublib.md
	${CMAKE_CURRENT_SOURCE_DIR}/doc/mscphublib_ru.md
	DESTINATION ${CMAKE_INSTALL_DOCDIR})

#
#	The manuals.  The markdown is the source and is always installed; the
#	PDF beside it is installed when it is there.  It is rendered on request
#	(MH_PDF): that costs pandoc and a TeX installation, which a host that
#	only runs the server does not need.  The rendered manuals are kept in
#	the repository with their sources, so the kit carries them and the
#	reader needs no TeX to read them.
#
set(MH_MANUALS	mscphub_users_guide		mscphub_users_guide_ru
		mscphub_programmers_guide	mscphub_programmers_guide_ru)

option(MH_PDF "Render the manuals as PDF from the markdown" OFF)

if ( MH_PDF )
	find_package(Python3 COMPONENTS Interpreter REQUIRED)
	find_program(MH_PANDOC	pandoc	REQUIRED)
	find_program(MH_XELATEX	xelatex	REQUIRED)
endif()

set(MH_PDFS)

foreach ( MH_MAN ${MH_MANUALS} )
	install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/doc/${MH_MAN}.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

	if ( MH_PDF )
		add_custom_command(
			OUTPUT	${CMAKE_CURRENT_SOURCE_DIR}/doc/${MH_MAN}.pdf
			COMMAND	${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/doc/mkpdf.py
				${CMAKE_CURRENT_SOURCE_DIR}/doc/${MH_MAN}.md:${CMAKE_CURRENT_SOURCE_DIR}/doc/${MH_MAN}.pdf
			DEPENDS	${CMAKE_CURRENT_SOURCE_DIR}/doc/${MH_MAN}.md ${CMAKE_CURRENT_SOURCE_DIR}/doc/mkpdf.py
			COMMENT	"Rendering doc/${MH_MAN}.pdf")

		list(APPEND MH_PDFS ${CMAKE_CURRENT_SOURCE_DIR}/doc/${MH_MAN}.pdf)
	endif()

	if ( MH_PDF OR EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/doc/${MH_MAN}.pdf )
		install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/doc/${MH_MAN}.pdf DESTINATION ${CMAKE_INSTALL_DOCDIR})
	endif()
endforeach()

if ( MH_PDF )
	add_custom_target(doc_pdf ALL DEPENDS ${MH_PDFS})
endif()

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

#
#	The .deb package: 'cpack -G DEB' in the build directory, built by
#	.github/workflows/release.yml.  The files go where the install step
#	puts them (/opt/mscphub, /etc/mscphub, /lib/systemd/system), hence
#	CPACK_SET_DESTDIR.  The unit is installed, never enabled.
#
set(CPACK_GENERATOR			"DEB")
set(CPACK_PACKAGE_NAME			"mscphub")
set(CPACK_PACKAGE_VENDOR		"StarLet Squad")
set(CPACK_PACKAGE_CONTACT		"Ruslan R. Laishev <zator@yandex.ru>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY	"MSCP disk server for OpenVMS nodes, over NISCA")
set(CPACK_SET_DESTDIR			ON)
set(CPACK_PACKAGING_INSTALL_PREFIX	"${CMAKE_INSTALL_PREFIX}")
set(CPACK_DEBIAN_FILE_NAME		DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE	1)
set(CPACK_DEBIAN_PACKAGE_SECTION	"net")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE	"https://github.com/SysMan-One/MSCP-Hub")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS	ON)
include(CPack)

if ( BUILD_TESTING )
	enable_testing()
	add_subdirectory(tests)
endif()
