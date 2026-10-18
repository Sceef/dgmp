# dgmp-server for Linux (g++ 10+ / clang 12+). The game client DLL and the launcher are Windows-only
# and are built by build.bat.
#   cmake -S . -B build-linux && cmake --build build-linux -j
# The binary lands next to server.ini and modes/ (like dgmp-server.exe); `cmake --install build-linux
# --prefix /opt/dgmp` copies the binary with the files it needs at runtime. See docs/server-linux.md.
cmake_minimum_required(VERSION 3.16)
project(dgmp-server C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)  # -O2 -g: crash stacks resolve with addr2line
endif()
option(DGMP_STATIC_RUNTIME "Link libstdc++ and libgcc statically (runs on older distributions)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- third-party static libraries ----
set(LUA_DIR ${CMAKE_SOURCE_DIR}/third_party/lua/src)
add_library(lua STATIC
  ${LUA_DIR}/lapi.c ${LUA_DIR}/lcode.c ${LUA_DIR}/lctype.c ${LUA_DIR}/ldebug.c ${LUA_DIR}/ldo.c ${LUA_DIR}/ldump.c
  ${LUA_DIR}/lfunc.c ${LUA_DIR}/lgc.c ${LUA_DIR}/llex.c ${LUA_DIR}/lmem.c ${LUA_DIR}/lobject.c ${LUA_DIR}/lopcodes.c
  ${LUA_DIR}/lparser.c ${LUA_DIR}/lstate.c ${LUA_DIR}/lstring.c ${LUA_DIR}/ltable.c ${LUA_DIR}/ltm.c ${LUA_DIR}/lundump.c
  ${LUA_DIR}/lvm.c ${LUA_DIR}/lzio.c ${LUA_DIR}/lauxlib.c ${LUA_DIR}/lbaselib.c ${LUA_DIR}/lcorolib.c ${LUA_DIR}/ldblib.c
  ${LUA_DIR}/liolib.c ${LUA_DIR}/lmathlib.c ${LUA_DIR}/loadlib.c ${LUA_DIR}/loslib.c ${LUA_DIR}/lstrlib.c
  ${LUA_DIR}/ltablib.c ${LUA_DIR}/lutf8lib.c ${LUA_DIR}/linit.c)
target_include_directories(lua PUBLIC ${LUA_DIR})
if(UNIX)
  target_compile_definitions(lua PRIVATE LUA_USE_POSIX)  # mkstemp, isatty; no dlopen: modes cannot load native code
  target_link_libraries(lua PUBLIC m)
endif()

set(ENET_DIR ${CMAKE_SOURCE_DIR}/third_party/enet)
if(WIN32)
  set(ENET_SOCKETS ${ENET_DIR}/win32.c)
else()
  set(ENET_SOCKETS ${ENET_DIR}/unix.c)
endif()
add_library(enet STATIC
  ${ENET_DIR}/callbacks.c ${ENET_DIR}/compress.c ${ENET_DIR}/host.c ${ENET_DIR}/list.c ${ENET_DIR}/packet.c
  ${ENET_DIR}/peer.c ${ENET_DIR}/protocol.c ${ENET_SOCKETS})
target_include_directories(enet PUBLIC ${ENET_DIR}/include)
if(WIN32)
  target_link_libraries(enet PUBLIC ws2_32 winmm)
else()
  # The configure step of ENet's own CMakeLists.txt, without its install rules.
  include(CheckFunctionExists)
  include(CheckStructHasMember)
  include(CheckTypeSize)
  foreach(fn fcntl poll getaddrinfo getnameinfo gethostbyname_r gethostbyaddr_r inet_pton inet_ntop)
    string(TOUPPER ${fn} FN)
    check_function_exists(${fn} HAS_${FN})
    if(HAS_${FN})
      target_compile_definitions(enet PRIVATE HAS_${FN}=1)
    endif()
  endforeach()
  check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" HAS_MSGHDR_FLAGS)
  set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h" "sys/socket.h")
  check_type_size("socklen_t" HAS_SOCKLEN_T BUILTIN_TYPES_ONLY)
  unset(CMAKE_EXTRA_INCLUDE_FILES)
  if(HAS_MSGHDR_FLAGS)
    target_compile_definitions(enet PRIVATE HAS_MSGHDR_FLAGS=1)
  endif()
  if(HAS_SOCKLEN_T)
    target_compile_definitions(enet PRIVATE HAS_SOCKLEN_T=1)
  endif()
endif()

# ---- dedicated server ----
find_package(Threads REQUIRED)
add_executable(dgmp-server src/server/main.cpp src/server/server.cpp src/server/script.cpp src/server/platform.cpp)
target_link_libraries(dgmp-server PRIVATE lua enet Threads::Threads)
if(NOT WIN32)  # on Windows build.bat owns dgmp-server.exe in the project folder
  set_target_properties(dgmp-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
if(MSVC)
  target_compile_options(dgmp-server PRIVATE /W4 /utf-8 /EHsc)
  target_compile_definitions(dgmp-server PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(dgmp-server PRIVATE dbghelp)
else()
  target_compile_options(dgmp-server PRIVATE -Wall -Wextra)
  target_link_options(dgmp-server PRIVATE -rdynamic)  # function names in crash stacks
  if(DGMP_STATIC_RUNTIME)
    target_link_options(dgmp-server PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()

# ---- cmake --install: a ready server folder ----
install(TARGETS dgmp-server RUNTIME DESTINATION .)
# server.ini only on the first install: an update keeps the configured one.
install(CODE "
  if(NOT EXISTS \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/server.ini\")
    file(INSTALL \"${CMAKE_SOURCE_DIR}/server.ini\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
  endif()")
install(DIRECTORY modes quests DESTINATION .)
install(FILES data/looks.txt DESTINATION data)
install(FILES docs/server-linux.md tools/dgmp-server.service DESTINATION docs OPTIONAL)  # a copy without them still installs
