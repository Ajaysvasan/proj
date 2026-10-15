# proj-agent.jar: the Java runtime agent behind `proj start dynamic`.
#
# Optional: built when a JDK (javac, jar) is found; without one, dynamic tracing is unavailable and
# everything else builds as before. ASM is downloaded as a pinned, hash-verified sources jar and
# compiled into the agent under proj.agent.asm, so it never clashes with an application's ASM.
# Offline builds: -DPROJ_ASM_SOURCES_JAR=/path/to/asm-9.10.1-sources.jar

find_package(Java 1.8 COMPONENTS Development)
if(NOT Java_JAVAC_EXECUTABLE OR NOT Java_JAR_EXECUTABLE)
  message(STATUS "proj-agent: no JDK found (javac, jar); the dynamic tracing agent is not built")
  return()
endif()

set(PROJ_ASM_URL https://repo1.maven.org/maven2/org/ow2/asm/asm/9.10.1/asm-9.10.1-sources.jar)
set(PROJ_ASM_SHA256 cb29bf42b4008972c566944d377192aa1b2d90275b9cfa9a7fa86de669fde11c)
set(PROJ_ASM_SOURCES_JAR "" CACHE FILEPATH "Local asm-9.10.1-sources.jar (skips the download)")

set(asm_jar ${PROJ_ASM_SOURCES_JAR})
if(NOT asm_jar)
  set(asm_jar ${CMAKE_CURRENT_BINARY_DIR}/asm-9.10.1-sources.jar)
  if(NOT EXISTS ${asm_jar})
    file(DOWNLOAD ${PROJ_ASM_URL} ${asm_jar}.part EXPECTED_HASH SHA256=${PROJ_ASM_SHA256} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE ${asm_jar}.part)
      message(FATAL_ERROR "proj-agent: downloading ${PROJ_ASM_URL} failed: ${status}")
    endif()
    file(RENAME ${asm_jar}.part ${asm_jar})
  endif()
endif()
file(SHA256 ${asm_jar} asm_hash)
if(NOT asm_hash STREQUAL PROJ_ASM_SHA256)
  message(FATAL_ERROR "proj-agent: ${asm_jar} does not have the expected SHA-256 ${PROJ_ASM_SHA256}")
endif()

file(GLOB_RECURSE proj_agent_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.java)
set(proj_agent_jar ${CMAKE_CURRENT_BINARY_DIR}/proj-agent.jar)
add_custom_command(
  OUTPUT ${proj_agent_jar}
  COMMAND ${CMAKE_COMMAND}
          -DASM_JAR=${asm_jar}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}
          -DJAVAC=${Java_JAVAC_EXECUTABLE}
          -DJAR=${Java_JAR_EXECUTABLE}
          -DJAVA_MAJOR=${Java_VERSION_MAJOR}
          -DVERSION=${PROJECT_VERSION}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/build_agent.cmake
  DEPENDS ${proj_agent_sources} ${asm_jar} ${CMAKE_CURRENT_SOURCE_DIR}/build_agent.cmake
          ${CMAKE_CURRENT_SOURCE_DIR}/ASM-LICENSE.txt
  COMMENT "Building proj-agent.jar"
  VERBATIM)
add_custom_target(proj_agent ALL DEPENDS ${proj_agent_jar})
set(PROJ_AGENT_JAR ${proj_agent_jar} PARENT_SCOPE)
