/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/c/eager/c_api.h"

#include <vector>

#include "tensorflow/c/c_api.h"
#include "tensorflow/c/eager/c_api_internal.h"
#ifdef TENSORFLOW_EAGER_USE_XLA
#include "tensorflow/compiler/jit/xla_device.h"
#endif  // TENSORFLOW_EAGER_USE_XLA

using tensorflow::int64;
using tensorflow::string;

namespace {

std::vector<int64> TensorShapeAsVector(const tensorflow::TensorHandle& handle,
                                       tensorflow::Status* status) {
  std::vector<int64> shape;
  int rank = -1;
  *status = handle.NumDims(&rank);
  if (!status->ok()) {
    return shape;
  }
  shape.reserve(rank);
  for (int i = 0; i < rank; ++i) {
    tensorflow::int64 dim;
    *status = handle.Dim(i, &dim);
    if (!status->ok()) {
      return shape;
    }
    shape.push_back(dim);
  }
  return shape;
}

}  // namespace

extern "C" {

TF_CAPI_EXPORT extern TFE_TensorDebugInfo* TFE_TensorHandleTensorDebugInfo(
    TFE_TensorHandle* h, TF_Status* status) {
  return h->handle->TensorDebugInfo(&status->status);
}

TFE_TensorDebugInfo* tensorflow::TensorHandleInterface::TensorDebugInfo(
    Status* status) {
  const tensorflow::Tensor* tensor;
  *status = handle_->Tensor(&tensor);
  if (!status->ok()) {
    return nullptr;
  }

#ifdef TENSORFLOW_EAGER_USE_XLA
  tensorflow::Device* device = handle_->device();

  // If tensor resides on an XLA device, use XLA device's PaddedShapeFn.
  tensorflow::XlaDevice* xla_device =
      dynamic_cast<tensorflow::XlaDevice*>(device);
  if (xla_device != nullptr) {
    tensorflow::XlaDevice::PaddedShapeFn shape_fn =
        xla_device->metadata().padded_shape_fn();
    xla::Shape padded_shape;
    *status = shape_fn(*tensor, &padded_shape);
    if (!status->ok()) {
      return nullptr;
    }
    if (VLOG_IS_ON(3)) {
      std::vector<int64> shape_to_log = TensorShapeAsVector(*handle_, status);
      if (!status->ok()) {
        // Ignore the status here as we are simply logging.
        *status = tensorflow::Status::OK();
      } else {
        VLOG(3) << "Fully padded shape of ["
                << absl::StrJoin(shape_to_log, ", ") << "] is "
                << padded_shape.DebugString();
      }
    }

    if (padded_shape.IsTuple()) {
      if (xla::ShapeUtil::TupleElementCount(padded_shape) != 2) {
        // Currently, the only case of XlaTensor containing a tuple shape is to
        // represent 64 bit ints, doubles, and complex numbers (we don't support
        // 64bit complex numbers).
        *status = tensorflow::errors::InvalidArgument(
            "XlaTensors should only contain tuples of size 2. Shape: ",
            padded_shape.DebugString());
        return nullptr;
      }

      // shape0 is not a const& because we will assign it to padded_shape below.
      // It is illegal to assign a part of a message to itself.
      xla::Shape shape0 = xla::ShapeUtil::GetTupleElementShape(padded_shape, 0);
      const xla::Shape& shape1 =
          xla::ShapeUtil::GetTupleElementShape(padded_shape, 1);
      if (shape0.IsTuple() || shape1.IsTuple()) {
        *status = tensorflow::errors::InvalidArgument(
            "XlaTensors should not contain nested tuples. Shape: ",
            padded_shape.DebugString());
        return nullptr;
      }
      if (!xla::ShapeUtil::Equal(shape0, shape1)) {
        *status = tensorflow::errors::InvalidArgument(
            "Subshapes of XlaTensors should be the same. Shape: ",
            padded_shape.DebugString());
        return nullptr;
      }

      // Since the only case we handle here are two equal subshapes, we
      // simply return one of them. The caller will interpret it as this
      // shape directly storing the 64bit types. This approximation is good
      // enough for this API's debugging use case.
      padded_shape = shape0;
    }

    int rank = padded_shape.dimensions_size();
    std::vector<int64> dev_dims;
    dev_dims.reserve(rank);
    if (rank == 1) {
      // Rank 1 tensors might not have padded_shape.layout.minor_to_major set,
      dev_dims.push_back(padded_shape.dimensions(0));
    } else {
      for (int i = rank - 1; i >= 0; --i) {
        int64 dim_index = padded_shape.layout().minor_to_major(i);
        dev_dims.push_back(padded_shape.dimensions(dim_index));
      }
    }
    *status = tensorflow::Status::OK();
    return new TFE_TensorDebugInfo(dev_dims);
  }
#endif  // TENSORFLOW_EAGER_USE_XLA

  // If the tensor is not an XLA tensor, the device shape is
  // the same as regular tensor shape.
  std::vector<int64> dev_dims = TensorShapeAsVector(*handle_, status);
  if (!status->ok()) {
    return nullptr;
  }
  return new TFE_TensorDebugInfo(dev_dims);
}

TF_CAPI_EXPORT extern void TFE_DeleteTensorDebugInfo(
    TFE_TensorDebugInfo* debug_info) {
  delete debug_info;
}

TF_CAPI_EXPORT extern int TFE_TensorDebugInfoOnDeviceNumDims(
    TFE_TensorDebugInfo* debug_info) {
  return debug_info->dev_dims.size();
}

TF_CAPI_EXPORT extern int64_t TFE_TensorDebugInfoOnDeviceDim(
    TFE_TensorDebugInfo* debug_info, int dim_index) {
  return debug_info->dev_dims[dim_index];
}

}  // extern "C"
