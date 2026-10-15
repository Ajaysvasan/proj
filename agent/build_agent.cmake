# cmake -P script: ASM sources (package renamed) + agent sources -> proj-agent.jar (Java 8 bytecode).
# Inputs: ASM_JAR SRC OUT JAVAC JAR JAVA_MAJOR VERSION. Works only inside OUT/agent-work.

set(work ${OUT}/agent-work)
file(REMOVE_RECURSE ${work})
file(MAKE_DIRECTORY ${work}/asm ${work}/src ${work}/classes/META-INF)

execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf ${ASM_JAR} WORKING_DIRECTORY ${work}/asm RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "proj-agent: cannot extract ${ASM_JAR}")
endif()

file(GLOB_RECURSE asm_sources RELATIVE ${work}/asm/org/objectweb/asm ${work}/asm/org/objectweb/asm/*.java)
set(sources "")
foreach(rel ${asm_sources})
  file(READ ${work}/asm/org/objectweb/asm/${rel} text)
  string(REPLACE "org.objectweb.asm" "proj.agent.asm" text "${text}")
  # Java 9 annotation syntax; plain @Deprecated means the same and compiles for Java 8.
  string(REPLACE "@Deprecated(forRemoval = false)" "@Deprecated" text "${text}")
  file(WRITE ${work}/src/proj/agent/asm/${rel} "${text}")
  string(APPEND sources "\"${work}/src/proj/agent/asm/${rel}\"\n")
endforeach()
file(GLOB_RECURSE agent_sources ${SRC}/src/*.java)
foreach(f ${agent_sources})
  string(APPEND sources "\"${f}\"\n")
endforeach()
file(WRITE ${work}/sources.txt "${sources}")

# Java 8 bytecode, so the agent loads into old JVMs as well.
if(JAVA_MAJOR GREATER_EQUAL 9)
  set(target_flags --release 8 -Xlint:-options)
else()
  set(target_flags -source 8 -target 8)
endif()
execute_process(
  COMMAND ${JAVAC} ${target_flags} -nowarn -encoding UTF-8 -d ${work}/classes @${work}/sources.txt
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "proj-agent: javac failed:\n${out}${err}")
endif()

file(COPY ${SRC}/ASM-LICENSE.txt DESTINATION ${work}/classes/META-INF)
file(WRITE ${work}/manifest.txt
  "Manifest-Version: 1.0\n"
  "Premain-Class: proj.agent.Agent\n"
  "Agent-Class: proj.agent.Agent\n"
  "Main-Class: proj.agent.Agent\n"
  "Can-Retransform-Classes: true\n"
  "Implementation-Title: proj-agent\n"
  "Implementation-Version: ${VERSION}\n")
execute_process(
  COMMAND ${JAR} cfm ${OUT}/proj-agent.jar.tmp ${work}/manifest.txt -C ${work}/classes .
  RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "proj-agent: jar failed: ${err}")
endif()
file(RENAME ${OUT}/proj-agent.jar.tmp ${OUT}/proj-agent.jar)
file(REMOVE_RECURSE ${work})
