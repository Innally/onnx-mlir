// RUN: onnx-mlir-opt %s

// COM: Enable when onnx-mlir driver works well with nnpa.
// COM: RUN_: onnx-mlir --printIR --EmitZHighIR --execNodesOnCpu=test/add0,test/add2 %s | FileCheck %s

func @test_add_force_cpu(%arg0 : tensor<10x10xf32>, %arg1 : tensor<10x10xf32>) -> tensor<*xf32> {
  %0 = "onnx.Add"(%arg0, %arg1) {onnx_node_name = "test/add0"} : (tensor<10x10xf32>, tensor<10x10xf32>) -> tensor<*xf32>
  %1 = "onnx.Add"(%0, %arg0) {onnx_node_name = "test/add1"} : (tensor<*xf32>, tensor<10x10xf32>) -> tensor<*xf32>
  %2 = "onnx.Add"(%1, %arg1) {onnx_node_name = "test/add2"} : (tensor<*xf32>, tensor<10x10xf32>) -> tensor<*xf32>
  "std.return"(%2) : (tensor<*xf32>) -> ()

  // CHECK-LABEL:  func @test_add_force_cpu
  // CHECK:           "onnx.Add"({{.*}}, {{.*}}) {onnx_node_name = "test/add0"}
  // CHECK:           "zhigh.Stick"
  // CHECK:           "zhigh.Stick"
  // CHECK:           "zhigh.Add"
  // CHECK:           "zhigh.Unstick"
  // CHECK:           "onnx.Add"({{.*}}, {{.*}}) {onnx_node_name = "test/add2"}
  // CHECK:           return
}

