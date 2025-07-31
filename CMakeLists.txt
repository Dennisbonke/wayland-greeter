cmake_minimum_required(VERSION 3.16)
project(wl_greeter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PAM REQUIRED pam)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)

add_executable(wl_greeter main.cpp)

target_include_directories(wl_greeter PRIVATE ${PAM_INCLUDE_DIRS} ${SYSTEMD_INCLUDE_DIRS})
# target_link_libraries(wl_greeter PRIVATE ${PAM_LIBRARIES})

target_link_libraries(wl_greeter PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg ${PAM_LIBRARIES} ${SYSTEMD_LIBRARIES})
target_compile_options(wl_greeter PRIVATE ${SYSTEMD_CFLAGS_OTHER})

add_executable(wl_greeter_wrapper main-wrapper.cpp)
target_include_directories(wl_greeter_wrapper PRIVATE ${PAM_INCLUDE_DIRS} ${SYSTEMD_INCLUDE_DIRS})
target_link_libraries(wl_greeter_wrapper PRIVATE ${PAM_LIBRARIES} ${SYSTEMD_LIBRARIES})
target_compile_options(wl_greeter_wrapper PRIVATE ${SYSTEMD_CFLAGS_OTHER})

# Install the wl_greeter binary into /usr/local/bin by default
install(TARGETS wl_greeter
    RUNTIME DESTINATION bin
)

install(TARGETS wl_greeter_wrapper
    RUNTIME DESTINATION bin
)