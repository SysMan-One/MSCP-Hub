#+++
#
#	FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
#
#	MODULE:		tests/CMakeLists.txt
#
#	ABSTRACT:	The regression tests: the wire formats against reference
#			frames of PEDRIVER, the protocol layers, the
#			parameters and the help library of the server.
#
#	AUTHOR:		StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
#
#	CREATION DATE:	17-OCT-2026
#
#	MODIFICATION HISTORY:
#
#		20-OCT-2026	RRL	X01-10 : Initial public version.
#		22-OCT-2026	RRL	X01-17 : the statistics block, Modbus, MHMON and T4.
#		22-OCT-2026	RRL	X01-18 : the scripts of OpenRC and System V init.
#		25-OCT-2026	RRL	X01-21 : the system disk of a satellite, T_BOOT.
#		25-OCT-2026	RRL	X01-22 : the reader of Files-11, T_ODS; the MOP server, T_MOP.
#
#---

add_executable(t_wire t_wire.c)

target_link_libraries(t_wire PRIVATE mhcore)

add_test(NAME wire COMMAND t_wire)

add_executable(t_vc t_vc.c)

target_link_libraries(t_vc PRIVATE mhcore)

add_test(NAME vc COMMAND t_vc)

add_executable(t_scs t_scs.c)

target_link_libraries(t_scs PRIVATE mhcore)

add_test(NAME scs COMMAND t_scs)

add_executable(t_boot t_boot.c)

target_link_libraries(t_boot PRIVATE mhcore)

add_test(NAME boot COMMAND t_boot)

add_executable(t_ods t_ods.c)

target_link_libraries(t_ods PRIVATE mhcore)

add_test(NAME ods COMMAND t_ods)

add_executable(t_mop t_mop.c)

target_link_libraries(t_mop PRIVATE mhcore)

add_test(NAME mop COMMAND t_mop)

add_executable(t_blk t_blk.c)

target_link_libraries(t_blk PRIVATE mhcore)

add_test(NAME blk COMMAND t_blk)

add_executable(t_bk t_bk.c)

target_link_libraries(t_bk PRIVATE mhcore)

add_test(NAME bk COMMAND t_bk)

add_executable(t_stat t_stat.c)

target_link_libraries(t_stat PRIVATE mhcore)

add_test(NAME stat COMMAND t_stat)

add_test(NAME t4 COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/t4.sh $<TARGET_FILE:t_stat> $<TARGET_FILE:mhmon>)

set_tests_properties(t4 PROPERTIES TIMEOUT 60)

add_test(NAME init COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/init.sh ${PROJECT_BINARY_DIR} $<TARGET_FILE:t_stat> $<TARGET_FILE:mhmon>)

set_tests_properties(init PROPERTIES TIMEOUT 60)

add_test(NAME params COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/params.sh $<TARGET_FILE:mscphub>)

add_test(NAME docs COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/docs.sh ${PROJECT_SOURCE_DIR})

if ( TARGET mscphublib )
	add_test(NAME helplib COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/helplib.sh $<TARGET_FILE:mscphub>
		${PROJECT_BINARY_DIR}/mscphub.hlb)
endif()
