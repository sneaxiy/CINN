add_definitions(${LLVM_DEFINITIONS})

# generate cinn_runtime.ll file

add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/cinn/backends/llvm/cinn_runtime_llvm_ir.h
  COMMAND ${LLVM_PATH}/bin/clang++ -mavx2 -std=c++11 -masm=intel -S -emit-llvm -O3 ${PROJECT_SOURCE_DIR}/cinn/runtime/cinn_runtime.cc -I${PROJECT_SOURCE_DIR} -o ${CMAKE_BINARY_DIR}/cinn/runtime/cinn_runtime.ll
  COMMAND python3 generate_runtime_llvm_ir.py ${CMAKE_BINARY_DIR}/cinn/runtime/cinn_runtime.ll ${CMAKE_BINARY_DIR}/cinn/backends/llvm/cinn_runtime_llvm_ir.h
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/cinn/backends/llvm
  DEPENDS ${PROJECT_SOURCE_DIR}/cinn/runtime/cinn_runtime.cc ${PROJECT_SOURCE_DIR}/cinn/runtime/cinn_runtime.h
  )
add_custom_target(GEN_LLVM_RUNTIME_IR_HEADER ALL
  DEPENDS ${CMAKE_BINARY_DIR}/cinn/backends/llvm/cinn_runtime_llvm_ir.h
  )

set(srcs
  llvm_util.cc
  runtime_symbol_registry.cc
  codegen_llvm.cc
  codegen_x86.cc
  simple_jit.cc
  execution_engine.cc
  llvm_optimizer.cc
)


cc_test(test_codegen_llvm SRCS codegen_llvm_test.cc DEPS cinncore)
cc_test(test_execution_engine SRCS execution_engine_test.cc DEPS cinncore)
cc_test(test_codegen_x86 SRCS codegen_x86_test.cc DEPS cinncore)

foreach(cpp ${srcs})
  set(cinnapi_src
    "${cinnapi_src};cinn/backends/llvm/${cpp}"
    CACHE INTERNAL "")
endforeach()

file(GLOB includes LIST_DIRECTORIES false RELATIVE ${CMAKE_SOURCE_DIR} *.h)

foreach(header ${includes})
  set(core_includes "${core_includes};${header}" CACHE INTERNAL "")
endforeach()
