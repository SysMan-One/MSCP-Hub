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

add_test(NAME params COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/params.sh $<TARGET_FILE:mscphub>)

add_test(NAME docs COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/docs.sh ${PROJECT_SOURCE_DIR})

if ( TARGET mscphublib )
	add_test(NAME helplib COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/helplib.sh $<TARGET_FILE:mscphub>
		${PROJECT_BINARY_DIR}/mscphub.hlb)
endif()
