# Project: Crankshaft
# This file is part of Crankshaft project.
# Copyright (C) 2025 OpenCarDev Team
#
#  Crankshaft is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 3 of the License, or
#  (at your option) any later version.
#
#  Crankshaft is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with Crankshaft. If not, see <http://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)

project(crankshaft-core)

include(GNUInstallDirs)

option(BUILD_TESTS "Build core unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules")
include(CrankshaftAasdk)

# Architecture detection for Debian packaging
if(TARGET_ARCH STREQUAL "amd64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
elseif(TARGET_ARCH STREQUAL "armhf")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "armhf")
elseif(TARGET_ARCH STREQUAL "arm64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
elseif(TARGET_ARCH STREQUAL "i386")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "i386")
else()
    # Auto-detect architecture for native builds
    execute_process(
        COMMAND dpkg --print-architecture
        OUTPUT_VARIABLE DETECTED_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(DETECTED_ARCH)
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${DETECTED_ARCH}")
        message(STATUS "Detected architecture: ${DETECTED_ARCH}")
    else()
        # Fallback to system processor
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
            set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7l")
            set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "armhf")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i386|i686")
            set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "i386")
        else()
            set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "unknown")
        endif()
        message(STATUS "Fallback architecture detection: ${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")
    endif()
endif()

# Include DEB package filename generation module
include(DebPackageFilename)

include(FetchContent)

# Prevent vendored dependencies from installing into the runtime DEB.
set(INSTALL_GTEST OFF CACHE BOOL "Disable gtest install rules" FORCE)
set(JSON_Install OFF CACHE BOOL "Disable nlohmann_json install rules" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "Disable nlohmann_json tests" FORCE)
set(JSON_SCHEMA_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "Disable json-schema-validator tests" FORCE)
set(JSON_SCHEMA_VALIDATOR_INSTALL OFF CACHE BOOL "Disable json-schema-validator install rules" FORCE)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# JSON library (nlohmann) and JSON Schema validator (pboettch)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.2
)
FetchContent_Declare(
  nlohmann_json_schema_validator
  GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
  # use the repository default branch to avoid tagging mismatch in CI images
  GIT_TAG main
)
FetchContent_MakeAvailable(nlohmann_json nlohmann_json_schema_validator)

find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets Gui DBus Bluetooth Sql Multimedia)

# Find GStreamer for multimedia
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GSTREAMER_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GSTREAMER_VIDEO REQUIRED gstreamer-video-1.0)
pkg_check_modules(GSTREAMER_AUDIO REQUIRED gstreamer-audio-1.0)

# Find DBus for wireless/bluetooth
pkg_check_modules(DBUS REQUIRED dbus-1)

# Find libusb for Android Auto
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# Find OpenSSL for AASDK
find_package(OpenSSL REQUIRED)

# Find AASDK (built externally by build.sh)
message(STATUS "Looking for AASDK system package")
if(NOT CRANKSHAFT_AASDK_FOUND)
  message(FATAL_ERROR "AASDK not found. Use --with-aasdk option in build.sh to build and install AASDK")
endif()

# When using installed AASDK, we need system protobuf
find_package(Protobuf REQUIRED)


# Core sources (excluding main.cpp)
set(CORE_SOURCES
  CoreApplicationRunner.cpp
  CoreApplicationRunner.h

  # Core Services
  services/eventbus/EventBus.cpp
  services/websocket/WebSocketServer.cpp
  services/config/ConfigService.cpp
  services/logging/Logger.cpp
  services/profile/ProfileManager.cpp
  services/service_manager/ServiceManager.cpp
  services/android_auto/AndroidAutoService.cpp
  services/android_auto/MockAndroidAutoService.cpp
  services/android_auto/RealAndroidAutoService.cpp
  services/android_auto/WiFiManagerWirelessNetworkManager.cpp
  services/android_auto/ProtocolHelpers.cpp
  services/preferences/PreferencesService.cpp
  services/session/SessionStore.cpp
  services/audio/AudioRouter.cpp
  services/bluetooth/BluetoothService.cpp
  services/media/MediaService.cpp
  services/extensions/ExtensionManager.cpp
  services/diagnostics/DiagnosticsEndpoint.cpp

  # Transport Layer
  hal/transport/Transport.cpp
  hal/transport/UARTTransport.cpp

  # Functional Device Layer
  hal/functional/FunctionalDevice.cpp
  hal/functional/GPSDevice.cpp
  hal/functional/CANDevice.cpp
  hal/functional/WiFiManagerImpl.cpp
  hal/functional/BluetoothManagerImpl.cpp

  # Mock Transports
  hal/mocks/transport/MockTransport.cpp

  # Multimedia HAL
  hal/multimedia/IVideoDecoder.cpp
  hal/multimedia/GStreamerVideoDecoder.cpp
  hal/multimedia/IAudioMixer.cpp
  hal/multimedia/AudioMixer.cpp
  hal/multimedia/AudioHAL.cpp
  hal/multimedia/VideoHAL.cpp
  hal/multimedia/MediaPipeline.cpp

  # Wireless HAL
  hal/wireless/WiFiHAL.cpp
  hal/wireless/BluetoothHAL.cpp
  hal/wireless/NetworkService.cpp
  hal/wireless/WiFiManager.cpp
  hal/wireless/BluetoothManager.cpp
)


# Library target for core logic
add_library(crankshaft-core-lib STATIC ${CORE_SOURCES})
# Ensure GStreamer include dirs are available to the core library
target_include_directories(crankshaft-core-lib PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${GSTREAMER_INCLUDE_DIRS}
  ${GSTREAMER_APP_INCLUDE_DIRS}
  ${GSTREAMER_VIDEO_INCLUDE_DIRS}
  ${GSTREAMER_AUDIO_INCLUDE_DIRS}
  ${DBUS_INCLUDE_DIRS}
  ${LIBUSB_INCLUDE_DIRS}
)

# Executable target (main.cpp only)
add_executable(crankshaft-core main.cpp)
target_link_libraries(crankshaft-core PRIVATE crankshaft-core-lib)


# Option to enable/disable CRANKSHAFT_AASDK_OLD_API macro
option(CRANKSHAFT_AASDK_OLD_API "Enable old AASDK API compatibility" ON)

if(CRANKSHAFT_AASDK_OLD_API)
  target_compile_definitions(crankshaft-core-lib PRIVATE CRANKSHAFT_AASDK_OLD_API)
  target_compile_definitions(crankshaft-core PRIVATE CRANKSHAFT_AASDK_OLD_API)
  # Propagate to all core-related targets (UI, tests, etc.)
  foreach(target crankshaft-core crankshaft-ui crankshaft-ui-slim crankshaft-tests)
    if(TARGET ${target})
      target_compile_definitions(${target} PRIVATE CRANKSHAFT_AASDK_OLD_API)
    endif()
  endforeach()
endif()

target_include_directories(crankshaft-core PRIVATE 
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${GSTREAMER_INCLUDE_DIRS}
  ${GSTREAMER_APP_INCLUDE_DIRS}
  ${GSTREAMER_VIDEO_INCLUDE_DIRS}
  ${GSTREAMER_AUDIO_INCLUDE_DIRS}
  ${DBUS_INCLUDE_DIRS}
  ${LIBUSB_INCLUDE_DIRS}
)

# Prefer a vendored json-schema-validator include under external/
if(EXISTS ${CMAKE_SOURCE_DIR}/external/json-schema-validator/include)
  target_include_directories(crankshaft-core PRIVATE
    ${CMAKE_SOURCE_DIR}/external/json-schema-validator/include
  )
  target_compile_definitions(crankshaft-core PRIVATE CRANKSHAFT_JSON_SCHEMA_VALIDATOR=1)
elseif(DEFINED nlohmann_json_schema_validator_SOURCE_DIR)
  # If FetchContent populated the json-schema-validator sources, add its include dir
  target_include_directories(crankshaft-core PRIVATE
    ${nlohmann_json_schema_validator_SOURCE_DIR}/include
  )
  # Signal to the code that the json-schema-validator headers are available
  target_compile_definitions(crankshaft-core PRIVATE CRANKSHAFT_JSON_SCHEMA_VALIDATOR=1)
else()
  # Not available in this build environment (CI may block network). Use fallback
  target_compile_definitions(crankshaft-core PRIVATE CRANKSHAFT_JSON_SCHEMA_VALIDATOR=0)
endif()

# Expose repo source dir to C++ so runtime can locate the schema files during development.
target_compile_definitions(crankshaft-core-lib PRIVATE CRANKSHAFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_definitions(crankshaft-core PRIVATE CRANKSHAFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set_target_properties(crankshaft-core-lib PROPERTIES
  AUTOMOC ON
)

set_target_properties(crankshaft-core PROPERTIES
  AUTOMOC ON
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/core
)

target_link_libraries(crankshaft-core-lib PRIVATE
  Qt6::Core
  Qt6::Network
  Qt6::WebSockets
  Qt6::Gui
  Qt6::DBus
  Qt6::Bluetooth
  Qt6::Sql
  Qt6::Multimedia
  ${GSTREAMER_LIBRARIES}
  ${GSTREAMER_APP_LIBRARIES}
  ${GSTREAMER_VIDEO_LIBRARIES}
  ${GSTREAMER_AUDIO_LIBRARIES}
  ${DBUS_LIBRARIES}
  ${LIBUSB_LIBRARIES}
  OpenSSL::SSL
  OpenSSL::Crypto
  protobuf::libprotobuf
  CRANKSHAFT_AASDK
  nlohmann_json::nlohmann_json
)
target_link_libraries(crankshaft-core PRIVATE crankshaft-core-lib)
  # Note: json-schema-validator is header-only and its include directories
  # are made available via FetchContent; no link target is required here.

# Install core executable
install(TARGETS crankshaft-core
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT core
)

# Install core configuration
install(FILES ${CMAKE_SOURCE_DIR}/config/crankshaft.json
  DESTINATION /etc/crankshaft
  COMPONENT core
)

# Add tests
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Install AA discovery trigger scripts
install(PROGRAMS
  ${CMAKE_SOURCE_DIR}/packaging/core/scripts/aa_discovery_trigger.sh
  ${CMAKE_SOURCE_DIR}/packaging/core/scripts/usb_action.sh
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/crankshaft
  COMPONENT core
)

# Install udev rules
install(FILES ${CMAKE_SOURCE_DIR}/packaging/core/udev/99-crankshaft.rules
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/udev/rules.d
  COMPONENT core
)

# Install systemd service unit
install(FILES ${CMAKE_SOURCE_DIR}/packaging/core/crankshaft-core.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
  COMPONENT core
)

# Ensure Debian package includes runtime shared-library dependencies.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libaasdk | libaasdk-amd64")
set(CPACK_COMPONENTS_ALL core)
