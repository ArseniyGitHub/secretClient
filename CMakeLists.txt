# CMakeList.txt: проект CMake для idk; включите исходный код и определения,
# укажите здесь логику для конкретного проекта.
#
cmake_minimum_required (VERSION 3.16)

if(DEFINED ENV{VCPKG_HOME})
    
    include("$ENV{VCPKG_HOME}/scripts/buildsystems/vcpkg.cmake")
else()
    include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()
set     (VCPKG_INCLUDE_DIR "$ENV{VCPKG_HOME}/installed/${VCPKG_TARGET_TRIPLET}/include")
set         (VCPKG_LIB_DIR "$ENV{VCPKG_HOME}/installed/${VCPKG_TARGET_TRIPLET}/lib")
set             (VCPKG_BIN_DIR "$ENV{VCPKG_HOME}/installed/${VCPKG_TARGET_TRIPLET}/bin")
set (VCPKG_DEBUG_BIN_DIR "$ENV{VCPKG_HOME}/installed/${VCPKG_TARGET_TRIPLET}/debug/bin")
set (VCPKG_DEBUG_LIB_DIR "$ENV{VCPKG_HOME}/installed/${VCPKG_TARGET_TRIPLET}/debug/lib")
include_directories(VCPKG_INCLUDE_DIR)
include_directories(${OPENSSL_INCLUDE_DIR})

# Включение горячей перезагрузки для компиляторов MSVC, если поддерживается.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project("idk")
find_package(Boost REQUIRED COMPONENTS asio uuid system thread)
find_package(SFML COMPONENTS Network Graphics Window Audio System CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# Добавьте источник в исполняемый файл этого проекта.
add_executable (client "v1/CLIENT/main.cpp")
add_executable (server "v1/SERVER/main.cpp")
add_executable (server_test "v1/SERVER/main2.cpp")

add_executable(server2 "v2/SERVER/main.cpp")
add_executable(client2 "v2/CLIENT/main.cpp")

target_link_libraries(client PRIVATE Boost::asio Boost::uuid Boost::system nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto ws2_32 crypt32 ${OPENSSL_LIBRARIES} Boost::thread kernel32.lib user32.lib gdi32.lib)
target_link_libraries(server PRIVATE Boost::asio Boost::uuid Boost::system nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto ws2_32 crypt32 ${OPENSSL_LIBRARIES} Boost::thread)
target_link_libraries(server_test PRIVATE Boost::asio Boost::uuid Boost::system nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto ws2_32 crypt32 ${OPENSSL_LIBRARIES} Boost::thread)

target_link_libraries(server2 PRIVATE Boost::asio Boost::uuid Boost::system nlohmann_json::nlohmann_json Boost::thread SFML::Network SFML::Graphics SFML::Window SFML::Audio SFML::System)
target_link_libraries(client2 PRIVATE Boost::asio Boost::uuid Boost::system nlohmann_json::nlohmann_json Boost::thread SFML::Network SFML::Graphics SFML::Window SFML::Audio SFML::System)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET client PROPERTY CXX_STANDARD 23)
  set_property(TARGET server PROPERTY CXX_STANDARD 23)
  set_property(TARGET server_test PROPERTY CXX_STANDARD 23)
  set_property(TARGET client2 PROPERTY CXX_STANDARD 23)
  set_property(TARGET server2 PROPERTY CXX_STANDARD 23)
endif()

# TODO: Добавьте тесты и целевые объекты, если это необходимо.
