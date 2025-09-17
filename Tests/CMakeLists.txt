if(BUILD_TESTS)

find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(
  JASPTest
  ${SYSTEM_TYPE}
  testall.h
  testall.cpp
  $<$<PLATFORM_ID:Darwin>:${_R_Framework}>
  $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/Desktop/JASP.exe.manifest>
)

add_dependencies(JASPTest JASPDesktopLib)


target_include_directories(
  JASPTest
    PUBLIC
    JASPDesktopLib
)


target_link_libraries(
  JASPTest
	PUBLIC
    JASPDesktopLib
    Qt::Test
  )


target_compile_definitions(
  JASPTest
  PUBLIC TESTLIBRARY_DIR=${CMAKE_SOURCE_DIR}/Tests/TestLibrary)

enable_testing(true)

add_test(NAME testall COMMAND testDataImports)


endif()
