get_property(OMLibs GLOBAL PROPERTY ONNX_MLIR_LIBS)

add_onnx_mlir_library(NNPACompilerUtils
  EXCLUDE_FROM_OM_LIBS
  NNPACompilerUtils.cpp

  INCLUDE_DIRS PRIVATE
  ${NNPA_SRC_ROOT}
  ${NNPA_BIN_ROOT}
  ${NNPA_ONNX_MLIR_SRC_ROOT}
  ${NNPA_ONNX_MLIR_BIN_ROOT}

  LINK_LIBS PUBLIC
  CompilerUtils
  ${OMLibs}

  ACCEL_INCLUDE_DIRS PRIVATE
  ${NNPA_ONNX_MLIR_SRC_ROOT}
  ${NNPA_ONNX_MLIR_BIN_ROOT}
  )
