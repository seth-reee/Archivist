cmake_minimum_required(VERSION 3.21)
project(Archivist VERSION 0.1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)

qt_add_executable(archivist main.cpp)
qt_add_resources(archivist "license" PREFIX "/" FILES LICENSE)
qt_add_resources(archivist "icon" PREFIX "/" FILES archivist.png)
target_compile_definitions(archivist PRIVATE ARCHIVIST_VERSION="${PROJECT_VERSION}")
target_link_libraries(archivist PRIVATE Qt6::Widgets PkgConfig::LIBARCHIVE)

enable_testing()
add_executable(archivist_tests tests.cpp)
target_compile_definitions(archivist_tests PRIVATE ARCHIVIST_VERSION="${PROJECT_VERSION}")
target_link_libraries(archivist_tests PRIVATE Qt6::Widgets PkgConfig::LIBARCHIVE)
add_test(NAME archivist_tests COMMAND archivist_tests)
set_tests_properties(archivist_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

install(TARGETS archivist RUNTIME DESTINATION bin)
install(FILES archivist.png DESTINATION share/pixmaps)
install(FILES archivist.desktop DESTINATION share/applications)
install(FILES LICENSE DESTINATION share/licenses/archivist)
