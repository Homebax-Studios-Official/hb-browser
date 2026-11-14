# HB Browser for Linux (GTK 3 + WebKitGTK). Build:  cmake -S platform/linux -B build-linux && cmake --build build-linux
# Needs: libgtk-3-dev libwebkit2gtk-4.1-dev libsoup-3.0-dev libsqlite3-dev nlohmann-json3-dev (see DOWNLOADS.md)
cmake_minimum_required(VERSION 3.16)
project(HBBrowserLinux VERSION 3.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.1)
pkg_check_modules(SOUP REQUIRED libsoup-3.0)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json QUIET)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/browser)
add_executable(hb-browser main.cpp
    ${CORE}/core/Util.cpp ${CORE}/core/Navigation.cpp ${CORE}/core/Settings.cpp
    ${CORE}/core/Storage.cpp ${CORE}/core/Security.cpp ${CORE}/core/Plugins.cpp)
target_include_directories(hb-browser PRIVATE ${CORE} ${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/nlohmann-json/include
    ${GTK3_INCLUDE_DIRS} ${WEBKIT_INCLUDE_DIRS} ${SOUP_INCLUDE_DIRS})
target_link_libraries(hb-browser PRIVATE ${GTK3_LIBRARIES} ${WEBKIT_LIBRARIES} ${SOUP_LIBRARIES} SQLite::SQLite3)
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(hb-browser PRIVATE nlohmann_json::nlohmann_json)
endif()
target_compile_options(hb-browser PRIVATE -Wall -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE)
target_link_options(hb-browser PRIVATE -pie -Wl,-z,relro,-z,now)
install(TARGETS hb-browser RUNTIME DESTINATION bin)
