// Copyright 2022 Google LLC
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <xnnpack.h>
#include <xnnpack/allocator.h>
#include <xnnpack/log.h>
#include <xnnpack/math.h>
#include <xnnpack/normalization.h>
#include <xnnpack/operator.h>

/// Reorder the data in array using the indices in loop_order.
///
/// Changing the loop order can have dramatic performance implications.
static void reorder_array(
    size_t num_dims,
    const size_t loop_order[restrict XNN_MIN_ELEMENTS(1) ],
    size_t array[restrict XNN_MIN_ELEMENTS(1)])
{
  size_t tmp[XNN_MAX_TENSOR_DIMS];
  memcpy(tmp, array, sizeof(size_t) * num_dims);
  for (size_t i = 0; i < num_dims; ++i) {
    array[i] = tmp[loop_order[i]];
  }
}

static enum xnn_status init_transpose_nd(
    uint32_t flags,
    uint32_t datatype_init_flags,
    enum xnn_operator_type operator_type,
    xnn_operator_t transpose_op)
{
  enum xnn_status status = xnn_status_unsupported_hardware;

  if ((xnn_params.init_flags & datatype_init_flags) != datatype_init_flags) {
    xnn_log_error(
      "failed to create %s operator: operations on data type are not supported",
      xnn_operator_type_to_string(operator_type));
    goto error;
  }
  transpose_op->flags = flags;
  transpose_op->type = operator_type;

  return xnn_status_success;

error:
  return status;
}

static enum xnn_status create_transpose_nd(
    uint32_t flags,
    uint32_t datatype_init_flags,
    enum xnn_operator_type operator_type,
    xnn_operator_t* transpose_op_out)
{
  enum xnn_status status = xnn_status_uninitialized;

  if ((xnn_params.init_flags & XNN_INIT_FLAG_XNNPACK) == 0) {
    xnn_log_error("failed to create %s operator: XNNPACK is not initialized",
      xnn_operator_type_to_string(operator_type));
    return status;
  }

  status = xnn_status_out_of_memory;
  xnn_operator_t transpose_op = xnn_allocate_zero_simd_memory(sizeof(struct xnn_operator));
  if (transpose_op == NULL) {
    xnn_log_error(
      "failed to allocate %zu bytes for %s operator descriptor",
      sizeof(struct xnn_operator), xnn_operator_type_to_string(operator_type));
    goto error;
  }

  status = init_transpose_nd(flags, datatype_init_flags, operator_type, transpose_op);
  if (status != xnn_status_success) {
    goto error;
  }
  *transpose_op_out = transpose_op;

  return xnn_status_success;

error:
  xnn_delete_operator(transpose_op);
  return status;
}

/// input_stride and output_stride are the number of elements between each
/// dimension, not the size of the dimension. This is because depth to space
/// splits the input channel dimension into three dimensions - block_size *
/// block_size * output_channels but gives input_channel_stride the stride over
/// all three dimensions. This must be multiplied by the product of the previous
/// dimensions to get the stride in elements. input_channel_stride is not
/// requried to be a multiple of block_size * block_size * output_channels so
/// the stride in number of elements must be supplied.
/// An interface for sub-tensors can easily be built on top of this.
static enum xnn_status setup_transpose_nd(
  xnn_operator_t transpose_op,
  const void* input,
  void* output,
  const size_t num_dims,
  const size_t* input_shape,
  const size_t* perm,
  const size_t* input_stride,
  const size_t* output_stride,
  size_t element_size)
{
  enum xnn_status status = xnn_status_invalid_parameter;
  transpose_op->state = xnn_run_state_invalid;

  if (num_dims == 0) {
    xnn_log_error(
      "failed to create %s operator with %zu num_dims: num_dims must be non-zero",
      xnn_operator_type_to_string(transpose_op->type), num_dims);
    goto error;
  }

  if (num_dims > XNN_MAX_TENSOR_DIMS) {
    xnn_log_error(
      "failed to create %s operator with %zu num_dims: num_dims must be <= %d",
      xnn_operator_type_to_string(transpose_op->type), num_dims, XNN_MAX_TENSOR_DIMS);
    goto error;
  }

  for (size_t i = 0; i < num_dims; ++i) {
    if (perm[i] >= num_dims) {
      xnn_log_error(
          "failed to create %s operator with %zu perm and %zu num_dims: 0 <= perm < num_dims",
          xnn_operator_type_to_string(transpose_op->type), perm[i], num_dims);
      goto error;
    }
  }

  for (size_t i = 0; i < num_dims - 1; ++i) {
    for (size_t j = i + 1; j < num_dims; ++j) {
      if (perm[i] == perm[j]) {
        xnn_log_error(
            "failed to create %s operator with duplicate entries in perm",
            xnn_operator_type_to_string(transpose_op->type));
        goto error;
      }
    }
  }

  if (input_stride != NULL) {
    if (input_stride[num_dims - 1] != 1) {
      xnn_log_error(
          "failed to create %s operator with %zu input_stride[num_dims - 1]: input_stride[num_dims - 1] == 1",
          xnn_operator_type_to_string(transpose_op->type), input_stride[num_dims - 1]);
    }
    size_t current_stride = 1;
    for (size_t i = num_dims - 1; i > 0; --i) {
      if ((input_stride[i - 1] < input_stride[i] * input_shape[i]) || (input_stride[i - 1] < current_stride)) {
        xnn_log_error(
            "failed to create %s operator with %zu input_shape and %zu input_stride: input_stride >= input_shape",
            xnn_operator_type_to_string(transpose_op->type), input_shape[i], input_stride[i]);
      }
      current_stride *= input_shape[i];
    }
  }

  if (output_stride != NULL) {
    if (output_stride[num_dims - 1] != 1) {
      xnn_log_error(
          "failed to create %s operator with %zu output_stride[num_dims - 1]: output_stride[num_dims - 1] == 1",
          xnn_operator_type_to_string(transpose_op->type), output_stride[num_dims - 1]);
    }
    size_t current_stride = 1;
    for (size_t i = num_dims - 1; i > 0; --i) {
      if ((output_stride[i - 1] < output_stride[i] * input_shape[perm[i]]) || (output_stride[i - 1] < current_stride)) {
        xnn_log_error(
            "failed to create %s operator with %zu output_shape and %zu output_stride: output_stride >= output_shape",
            xnn_operator_type_to_string(transpose_op->type), input_shape[perm[i]], output_stride[i]);
      }
      current_stride *= input_shape[perm[i]];
    }
  }

  // Early exit without setting up context if any shape dimension is zero.
  bool degenerate_shape = false;
  for (size_t i = 0; i < num_dims; ++i) {
    degenerate_shape |= input_shape[i] == 0;
  }

  if (degenerate_shape) {
    transpose_op->state = xnn_run_state_skip;
    return xnn_status_success;
  }

  transpose_op->channels = num_dims;

  struct transpose_context* context = &transpose_op->context.transpose;
  size_t normalized_dims;
  size_t normalized_shape[XNN_MAX_TENSOR_DIMS];
  size_t normalized_perm[XNN_MAX_TENSOR_DIMS];
  size_t normalized_element_size;
  xnn_normalize_transpose_permutation(num_dims, element_size, perm, input_shape, input_stride, output_stride, &normalized_dims,
                                      &normalized_element_size, normalized_perm, normalized_shape, context->input_stride, context->output_stride);

  size_t loop_order[XNN_MAX_TENSOR_DIMS];
  memcpy(loop_order, normalized_perm, sizeof(size_t) * normalized_dims);

  /// The innermost loop must iterate over the contiguous input dimension and the second most inner loop over the
  /// contiguous output dimension.
  if (normalized_dims > 1) {
    for (size_t i = 0; i < normalized_dims - 2; ++i) {
      if (loop_order[i] == normalized_dims - 1) {
        size_t tmp = loop_order[i];
        loop_order[i] = loop_order[normalized_dims - 2];
        loop_order[normalized_dims - 2] = tmp;
        tmp = context->output_stride[i];
        context->output_stride[i] = context->output_stride[normalized_dims - 2];
        context->output_stride[normalized_dims - 2] = tmp;
        break;
      }
    }
  }

  for (size_t i = 0; i < normalized_dims; ++i) {
    transpose_op->compute.range[i] = normalized_shape[i];
  }
  reorder_array(normalized_dims, loop_order, context->input_stride);
  reorder_array(normalized_dims, loop_order, transpose_op->compute.range);

  bool variable_size_ukernel = false;
  switch (normalized_element_size) {
    case 1:
      context->log2_element_size = 0;
      context->const_size_ukernel = xnn_params.x8.transpose.const_size_ukernel;
      transpose_op->compute.tile[0] = xnn_params.x8.transpose.tile_size;
      transpose_op->compute.tile[1] = xnn_params.x8.transpose.tile_size;
      break;
    case 2:
      context->log2_element_size = 1;
      transpose_op->compute.tile[0] = xnn_params.x16.transpose.tile_size;
      transpose_op->compute.tile[1] = xnn_params.x16.transpose.tile_size;
      context->const_size_ukernel = xnn_params.x16.transpose.const_size_ukernel;
      break;
    case 4:
      context->log2_element_size = 2;
      transpose_op->compute.tile[0] = xnn_params.x32.transpose.tile_size;
      transpose_op->compute.tile[1] = xnn_params.x32.transpose.tile_size;
      context->const_size_ukernel = xnn_params.x32.transpose.const_size_ukernel;
      break;
    default:
      context->element_size = normalized_element_size;
      transpose_op->compute.tile[0] = xnn_params.xx.transpose.tile_size;
      transpose_op->compute.tile[1] = xnn_params.xx.transpose.tile_size;
      context->variable_size_ukernel = xnn_params.xx.transpose.variable_size_ukernel;
      variable_size_ukernel = true;
  }

  struct univector_contiguous_context* univector_context = &transpose_op->context.univector_contiguous;
  switch (normalized_dims) {
    case 1:
      transpose_op->compute.type = xnn_parallelization_type_1d_tile_1d;
      transpose_op->compute.task_1d = (pthreadpool_task_1d_t) xnn_compute_univector_contiguous;
      transpose_op->compute.range[0] = normalized_element_size;
      univector_context->ukernel = xnn_params.xx.copy;
      univector_context->log2_xsize = 0;
      univector_context->log2_ysize = 0;
      break;
    case 2:
      transpose_op->compute.type = xnn_parallelization_type_2d_tile_2d;
      if (variable_size_ukernel) {
        transpose_op->compute.task_2d_tile_2d = (pthreadpool_task_2d_tile_2d_t) xnn_compute_transposev_2d;
      } else {
        transpose_op->compute.task_2d_tile_2d = (pthreadpool_task_2d_tile_2d_t) xnn_compute_transposec_2d;
      }
      break;
    case 3:
      transpose_op->compute.type = xnn_parallelization_type_3d_tile_2d;
      if (variable_size_ukernel) {
        transpose_op->compute.task_3d_tile_2d = (pthreadpool_task_3d_tile_2d_t) xnn_compute_transposev_3d;
      } else {
        transpose_op->compute.task_3d_tile_2d = (pthreadpool_task_3d_tile_2d_t) xnn_compute_transposec_3d;
      }
      break;
    case 4:
      transpose_op->compute.type = xnn_parallelization_type_4d_tile_2d;
      if (variable_size_ukernel) {
        transpose_op->compute.task_4d_tile_2d = (pthreadpool_task_4d_tile_2d_t) xnn_compute_transposev_4d;
      } else {
        transpose_op->compute.task_4d_tile_2d = (pthreadpool_task_4d_tile_2d_t) xnn_compute_transposec_4d;
      }
      break;
    case 5:
      transpose_op->compute.type = xnn_parallelization_type_5d_tile_2d;
      if (variable_size_ukernel) {
        transpose_op->compute.task_5d_tile_2d = (pthreadpool_task_5d_tile_2d_t) xnn_compute_transposev_5d;
      } else {
        transpose_op->compute.task_5d_tile_2d = (pthreadpool_task_5d_tile_2d_t) xnn_compute_transposec_5d;
      }
      break;
    case 6:
      transpose_op->compute.type = xnn_parallelization_type_6d_tile_2d;
      if (variable_size_ukernel) {
        transpose_op->compute.task_6d_tile_2d = (pthreadpool_task_6d_tile_2d_t) xnn_compute_transposev_6d;
      } else {
        transpose_op->compute.task_6d_tile_2d = (pthreadpool_task_6d_tile_2d_t) xnn_compute_transposec_6d;
      }
      break;
    default:
      XNN_UNREACHABLE;
  }

  if (transpose_op->channels == 1) {
    transpose_op->context.univector_contiguous.x = input;
    transpose_op->context.univector_contiguous.y = output;
  } else {
    transpose_op->context.transpose.x = input;
    transpose_op->context.transpose.y = output;
  }
  transpose_op->state = xnn_run_state_ready;

  return xnn_status_success;

error:
  xnn_delete_operator(transpose_op);
  return status;
}

enum xnn_status xnn_create_transpose_nd_x32(
  uint32_t flags,
  xnn_operator_t* transpose_op_out)
{
  return create_transpose_nd(
    flags,
    XNN_INIT_FLAG_X32,
    xnn_operator_type_transpose_nd_x32,
    transpose_op_out);
}

enum xnn_status xnn_create_transpose_nd_x16(
  uint32_t flags,
  xnn_operator_t* transpose_op_out)
{
  return create_transpose_nd(
    flags,
    XNN_INIT_FLAG_X16,
    xnn_operator_type_transpose_nd_x16,
    transpose_op_out);
}

enum xnn_status xnn_create_transpose_nd_x8(
  uint32_t flags,
  xnn_operator_t* transpose_op_out)
{
  return create_transpose_nd(
    flags,
    XNN_INIT_FLAG_X8,
    xnn_operator_type_transpose_nd_x8,
    transpose_op_out);
}

enum xnn_status xnn_setup_transpose_nd_x32(
    xnn_operator_t transpose_op,
    const void* input,
    void* output,
    size_t num_dims,
    const size_t* shape,
    const size_t* perm,
    pthreadpool_t threadpool)
{
  if (transpose_op->type != xnn_operator_type_transpose_nd_x32) {
    xnn_log_error("failed to setup operator: operator type mismatch (expected %s, got %s)",
      xnn_operator_type_to_string(xnn_operator_type_transpose_nd_x32),
      xnn_operator_type_to_string(transpose_op->type));
    return xnn_status_invalid_parameter;
  }

  return setup_transpose_nd(
    transpose_op,
    input, output,
    num_dims, shape, perm, NULL, NULL,
    sizeof(uint32_t));
}

enum xnn_status xnn_setup_transpose_nd_x16(
    xnn_operator_t transpose_op,
    const void* input,
    void* output,
    size_t num_dims,
    const size_t* shape,
    const size_t* perm,
    pthreadpool_t threadpool)
{
  if (transpose_op->type != xnn_operator_type_transpose_nd_x16) {
    xnn_log_error("failed to setup operator: operator type mismatch (expected %s, got %s)",
      xnn_operator_type_to_string(xnn_operator_type_transpose_nd_x16),
      xnn_operator_type_to_string(transpose_op->type));
    return xnn_status_invalid_parameter;
  }

  return setup_transpose_nd(
    transpose_op,
    input, output,
    num_dims, shape, perm, NULL, NULL,
    sizeof(uint16_t));
}

enum xnn_status xnn_setup_transpose_nd_x8(
    xnn_operator_t transpose_op,
    const void* input,
    void* output,
    size_t num_dims,
    const size_t* shape,
    const size_t* perm,
    pthreadpool_t threadpool)
{
  if (transpose_op->type != xnn_operator_type_transpose_nd_x8) {
    xnn_log_error("failed to setup operator: operator type mismatch (expected %s, got %s)",
      xnn_operator_type_to_string(xnn_operator_type_transpose_nd_x8),
      xnn_operator_type_to_string(transpose_op->type));
    return xnn_status_invalid_parameter;
  }

  return setup_transpose_nd(
    transpose_op,
    input, output,
    num_dims, shape, perm, NULL, NULL,
    sizeof(uint8_t));
}

enum xnn_status run_transpose_nd(
    uint32_t flags,
    const void* input,
    void* output,
    const size_t num_dims,
    const size_t* input_shape,
    const size_t* output_perm,
    size_t element_size,
    uint32_t datatype_init_flags,
    enum xnn_operator_type operator_type,
    pthreadpool_t threadpool) {
  enum xnn_status status = xnn_status_uninitialized;

  if ((xnn_params.init_flags & XNN_INIT_FLAG_XNNPACK) == 0) {
    xnn_log_error("failed to create %s operator: XNNPACK is not initialized",
      xnn_operator_type_to_string(operator_type));
    return status;
  }

  struct xnn_operator transpose_op;
  memset(&transpose_op, 0, sizeof(transpose_op));

  status = init_transpose_nd(
      flags,
      datatype_init_flags,
      operator_type,
      &transpose_op);
  if (status != xnn_status_success) {
    return status;
  }

  status = setup_transpose_nd(&transpose_op,
                              input,
                              output,
                              num_dims,
                              input_shape,
                              output_perm,
                              NULL,
                              NULL,
                              element_size);
  if (status != xnn_status_success) {
    return status;
  }

  return xnn_run_operator(&transpose_op, threadpool);
}

enum xnn_status xnn_run_transpose_nd_x32(
    const void* input,
    void* output,
    const size_t num_dims,
    const size_t* input_shape,
    const size_t* output_perm,
    uint32_t flags,
    pthreadpool_t threadpool)
{
  return run_transpose_nd(
    flags,
    input,
    output,
    num_dims,
    input_shape,
    output_perm,
    sizeof(uint32_t),
    XNN_INIT_FLAG_X32,
    xnn_operator_type_transpose_nd_x32,
    threadpool);
}

enum xnn_status xnn_run_transpose_nd_x16(
    const void* input,
    void* output,
    const size_t num_dims,
    const size_t* input_shape,
    const size_t* output_perm,
    uint32_t flags,
    pthreadpool_t threadpool)
{
  return run_transpose_nd(
    flags,
    input,
    output,
    num_dims,
    input_shape,
    output_perm,
    sizeof(uint16_t),
    XNN_INIT_FLAG_X16,
    xnn_operator_type_transpose_nd_x16,
    threadpool);
}

enum xnn_status xnn_run_transpose_nd_x8(
    const void* input,
    void* output,
    const size_t num_dims,
    const size_t* input_shape,
    const size_t* output_perm,
    uint32_t flags,
    pthreadpool_t threadpool)
{
  return run_transpose_nd(
    flags,
    input,
    output,
    num_dims,
    input_shape,
    output_perm,
    sizeof(uint8_t),
    XNN_INIT_FLAG_X8,
    xnn_operator_type_transpose_nd_x8,
    threadpool);
}

enum xnn_status xnn_create_depth_to_space_nchw2nhwc_x32(
    size_t output_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    xnn_operator_t* depth_to_space_op_out)
{
  xnn_operator_t depth_to_space_op = NULL;
  enum xnn_status status = xnn_status_uninitialized;

  if ((xnn_params.init_flags & XNN_INIT_FLAG_XNNPACK) == 0) {
    xnn_log_error("failed to create %s operator: XNNPACK is not initialized",
      xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32));
    goto error;
  }

  status = xnn_status_invalid_parameter;

  if (output_channels == 0) {
    xnn_log_error("failed to create %s operator with %zu output channels: number of channels must be non-zero",
      xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32), output_channels);
    goto error;
  }

  if (output_channel_stride < output_channels) {
    xnn_log_error(
      "failed to create %s operator with output channel stride of %zu: "
      "stride must be at least as large as the number of output channels (%zu)",
      xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32),
      output_channel_stride, output_channels);
    goto error;
  }

  if (block_size <= 1) {
    xnn_log_error("failed to create %s operator with %" PRIu32 " block size: block size must be greater than 1",
      xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32),
      block_size);
    goto error;
  }

  const size_t input_channels = output_channels * block_size * block_size;
  if (input_channel_stride < input_channels) {
    xnn_log_error(
      "failed to create %s operator with input channel stride of %zu: "
      "stride must be at least as large as the number of input channels (%" PRIu32 "x%" PRIu32 "x%zu)",
      xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32),
      input_channel_stride, block_size, block_size, input_channels);
    goto error;
  }

  status = xnn_status_out_of_memory;

  depth_to_space_op = xnn_allocate_zero_simd_memory(sizeof(struct xnn_operator));
  if (depth_to_space_op == NULL) {
    xnn_log_error(
      "failed to allocate %zu bytes for %s operator descriptor",
      sizeof(struct xnn_operator), xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32));
    goto error;
  }

  depth_to_space_op->channels = output_channels;
  depth_to_space_op->input_pixel_stride = input_channel_stride;
  depth_to_space_op->output_pixel_stride = output_channel_stride;
  depth_to_space_op->block_size = block_size;

  depth_to_space_op->type = xnn_operator_type_depth_to_space_nchw2nhwc_x32;
  depth_to_space_op->flags = flags;

  depth_to_space_op->state = xnn_run_state_invalid;

  *depth_to_space_op_out = depth_to_space_op;
  return xnn_status_success;

error:
  xnn_delete_operator(depth_to_space_op);
  return status;
}

enum xnn_status xnn_setup_depth_to_space_nchw2nhwc_x32(
    xnn_operator_t depth_to_space_op,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    pthreadpool_t threadpool)
{
  if (depth_to_space_op->type != xnn_operator_type_depth_to_space_nchw2nhwc_x32) {
    xnn_log_error("failed to setup operator: operator type mismatch (expected %s, got %s)",
      xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32),
      xnn_operator_type_to_string(depth_to_space_op->type));
    return xnn_status_invalid_parameter;
  }
  depth_to_space_op->state = xnn_run_state_invalid;

  if ((xnn_params.init_flags & XNN_INIT_FLAG_XNNPACK) == 0) {
    xnn_log_error("failed to setup %s operator: XNNPACK is not initialized",
      xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32));
    return xnn_status_uninitialized;
  }

  if (input_width == 0 || input_height == 0) {
    xnn_log_error("failed to setup %s operator with %zux%zu input: input dimensions must be non-zero",
      xnn_operator_type_to_string(xnn_operator_type_depth_to_space_nchw2nhwc_x32), input_width, input_height);
    return xnn_status_invalid_parameter;
  }

  if (batch_size == 0) {
    depth_to_space_op->state = xnn_run_state_skip;
    return xnn_status_success;
  }

  const uint32_t block_size = depth_to_space_op->block_size;
  const size_t channels = depth_to_space_op->channels;

  const size_t input_shape[6] = {batch_size, block_size, block_size, channels, input_height, input_width};
  const size_t perm[6] = {0, 4, 1, 5, 2, 3};
  const size_t area = input_height * input_width;
  const size_t elements_per_batch = area * channels;
  const size_t input_stride[6] = {
    depth_to_space_op->input_pixel_stride * area,
    block_size * elements_per_batch,
    elements_per_batch,
    area,
    input_width,
    1};
  const size_t output_stride[6] = {
    input_height * block_size * input_width * block_size * depth_to_space_op->output_pixel_stride,
    block_size * input_width * block_size * depth_to_space_op->output_pixel_stride,
    input_width * block_size * depth_to_space_op->output_pixel_stride,
    block_size * depth_to_space_op->output_pixel_stride,
    depth_to_space_op->output_pixel_stride,
    1};

  return setup_transpose_nd(
    depth_to_space_op,
    input,
    output,
    6,
    input_shape,
    perm,
    input_stride,
    output_stride,
    sizeof(uint32_t));
}

static enum xnn_status create_depth_to_space_nhwc(
    size_t output_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    enum xnn_operator_type operator_type,
    xnn_operator_t* depth_to_space_op_out)
{
  xnn_operator_t depth_to_space_op = NULL;
  enum xnn_status status = xnn_status_uninitialized;

  if ((xnn_params.init_flags & XNN_INIT_FLAG_XNNPACK) == 0) {
    xnn_log_error("failed to create %s operator: XNNPACK is not initialized",
      xnn_operator_type_to_string(operator_type));
    goto error;
  }

  status = xnn_status_invalid_parameter;

  if (output_channels == 0) {
    xnn_log_error("failed to create %s operator with %zu output channels: number of channels must be non-zero",
      xnn_operator_type_to_string(operator_type), output_channels);
    goto error;
  }

  if (output_channel_stride < output_channels) {
    xnn_log_error(
      "failed to create %s operator with output channel stride of %zu: "
      "stride must be at least as large as the number of output channels (%zu)",
      xnn_operator_type_to_string(operator_type),
      output_channel_stride, output_channels);
    goto error;
  }

  if (block_size <= 1) {
    xnn_log_error("failed to create %s operator with %" PRIu32 " block size: block size must be greater than 1",
      xnn_operator_type_to_string(operator_type),
      block_size);
    goto error;
  }

  const size_t input_channels = output_channels * block_size * block_size;
  if (input_channel_stride < input_channels) {
    xnn_log_error(
      "failed to create %s operator with input channel stride of %zu: "
      "stride must be at least as large as the number of input channels (%" PRIu32 "x%" PRIu32 "x%zu)",
      xnn_operator_type_to_string(operator_type),
      input_channel_stride, block_size, block_size, input_channels);
    goto error;
  }

  status = xnn_status_out_of_memory;

  depth_to_space_op = xnn_allocate_zero_simd_memory(sizeof(struct xnn_operator));
  if (depth_to_space_op == NULL) {
    xnn_log_error(
      "failed to allocate %zu bytes for %s operator descriptor",
      sizeof(struct xnn_operator), xnn_operator_type_to_string(operator_type));
    goto error;
  }

  depth_to_space_op->channels = output_channels;
  depth_to_space_op->input_pixel_stride = input_channel_stride;
  depth_to_space_op->output_pixel_stride = output_channel_stride;
  depth_to_space_op->block_size = block_size;

  depth_to_space_op->type = operator_type;
  depth_to_space_op->flags = flags;

  depth_to_space_op->state = xnn_run_state_invalid;

  *depth_to_space_op_out = depth_to_space_op;
  return xnn_status_success;

error:
  xnn_delete_operator(depth_to_space_op);
  return status;
}

enum xnn_status xnn_create_depth_to_space_nhwc_x8(
    size_t output_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    xnn_operator_t* depth_to_space_op_out)
{
  return create_depth_to_space_nhwc(
    output_channels,
    input_channel_stride,
    output_channel_stride,
    block_size,
    flags,
    xnn_operator_type_depth_to_space_nhwc_x8,
    depth_to_space_op_out);
}

enum xnn_status xnn_create_depth_to_space_nhwc_x16(
    size_t output_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    xnn_operator_t* depth_to_space_op_out)
{
  return create_depth_to_space_nhwc(
    output_channels,
    input_channel_stride,
    output_channel_stride,
    block_size,
    flags,
    xnn_operator_type_depth_to_space_nhwc_x16,
    depth_to_space_op_out);
}

enum xnn_status xnn_create_depth_to_space_nhwc_x32(
    size_t output_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    xnn_operator_t* depth_to_space_op_out)
{
  return create_depth_to_space_nhwc(
    output_channels,
    input_channel_stride,
    output_channel_stride,
    block_size,
    flags,
    xnn_operator_type_depth_to_space_nhwc_x32,
    depth_to_space_op_out);
}

static enum xnn_status setup_depth_to_space_nhwc(
    xnn_operator_t depth_to_space_op,
    enum xnn_operator_type expected_operator_type,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    uint32_t element_size)
{
  if (depth_to_space_op->type != expected_operator_type) {
    xnn_log_error("failed to setup operator: operator type mismatch (expected %s, got %s)",
      xnn_operator_type_to_string(expected_operator_type),
      xnn_operator_type_to_string(depth_to_space_op->type));
    return xnn_status_invalid_parameter;
  }
  depth_to_space_op->state = xnn_run_state_invalid;

  if ((xnn_params.init_flags & XNN_INIT_FLAG_XNNPACK) == 0) {
    xnn_log_error("failed to setup %s operator: XNNPACK is not initialized",
      xnn_operator_type_to_string(expected_operator_type));
    return xnn_status_uninitialized;
  }

  if (input_width == 0 || input_height == 0) {
    xnn_log_error("failed to setup %s operator with %zux%zu input: input dimensions must be non-zero",
      xnn_operator_type_to_string(expected_operator_type), input_width, input_height);
    return xnn_status_invalid_parameter;
  }

  if (batch_size == 0) {
    depth_to_space_op->state = xnn_run_state_skip;
    return xnn_status_success;
  }

  const uint32_t block_size = depth_to_space_op->block_size;
  const size_t channels = depth_to_space_op->channels;
  const size_t input_pixel_stride = depth_to_space_op->input_pixel_stride;
  const size_t output_pixel_stride = depth_to_space_op->output_pixel_stride;
  const size_t block_output_pixel_stride = block_size * depth_to_space_op->output_pixel_stride;

  const size_t input_shape[5] = {batch_size * input_height, input_width, block_size, block_size, channels};
  const size_t perm[5] = {0, 2, 1, 3, 4};
  const size_t input_stride[5] = {
    input_width * input_pixel_stride,
    input_pixel_stride,
    block_size * channels,
    channels,
    1};
  const size_t output_stride[5] = {
    block_size * input_width * block_output_pixel_stride,
    input_width * block_output_pixel_stride,
    block_output_pixel_stride,
    output_pixel_stride,
    1};

  return setup_transpose_nd(
      depth_to_space_op,
      input,
      output,
      5,
      input_shape,
      perm,
      input_stride,
      output_stride,
      element_size);
}

enum xnn_status xnn_setup_depth_to_space_nhwc_x8(
    xnn_operator_t depth_to_space_op,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    pthreadpool_t threadpool)
{
  return setup_depth_to_space_nhwc(
    depth_to_space_op,
    xnn_operator_type_depth_to_space_nhwc_x8,
    batch_size, input_height, input_width,
    input, output, 1);
}

enum xnn_status xnn_setup_depth_to_space_nhwc_x16(
    xnn_operator_t depth_to_space_op,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    pthreadpool_t threadpool)
{
  return setup_depth_to_space_nhwc(
    depth_to_space_op,
    xnn_operator_type_depth_to_space_nhwc_x16,
    batch_size, input_height, input_width,
    input, output, 2);
}

enum xnn_status xnn_setup_depth_to_space_nhwc_x32(
    xnn_operator_t depth_to_space_op,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    pthreadpool_t threadpool)
{
  return setup_depth_to_space_nhwc(
    depth_to_space_op,
    xnn_operator_type_depth_to_space_nhwc_x32,
    batch_size, input_height, input_width,
    input, output, 4);
}

static enum xnn_status create_space_to_depth_nhwc(
    size_t input_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    enum xnn_operator_type operator_type,
    xnn_operator_t* space_to_depth_op_out)
{
  xnn_operator_t space_to_depth_op = NULL;
  enum xnn_status status = xnn_status_uninitialized;

  if ((xnn_params.init_flags & XNN_INIT_FLAG_XNNPACK) == 0) {
    xnn_log_error("failed to create %s operator: XNNPACK is not initialized",
      xnn_operator_type_to_string(operator_type));
    goto error;
  }

  status = xnn_status_invalid_parameter;

  if (input_channels == 0) {
    xnn_log_error("failed to create %s operator with %zu input channels: number of channels must be non-zero",
      xnn_operator_type_to_string(operator_type), input_channels);
    goto error;
  }

  if (input_channel_stride < input_channels) {
    xnn_log_error(
      "failed to create %s operator with input channel stride of %zu: "
      "stride must be at least as large as the number of input channels (%zu)",
      xnn_operator_type_to_string(operator_type),
      input_channel_stride, input_channels);
    goto error;
  }

  if (block_size <= 1) {
    xnn_log_error("failed to create %s operator with %" PRIu32 " block size: block size must be greater than 1",
      xnn_operator_type_to_string(operator_type),
      block_size);
    goto error;
  }

  const size_t output_channels = input_channels * block_size * block_size;
  if (output_channel_stride < output_channels) {
    xnn_log_error(
      "failed to create %s operator with output channel stride of %zu: "
      "stride must be at least as large as the number of output channels (%" PRIu32 "x%" PRIu32 "x%zu)",
      xnn_operator_type_to_string(operator_type),
      output_channel_stride, block_size, block_size, input_channels);
    goto error;
  }

  status = xnn_status_out_of_memory;

  space_to_depth_op = xnn_allocate_zero_simd_memory(sizeof(struct xnn_operator));
  if (space_to_depth_op == NULL) {
    xnn_log_error(
      "failed to allocate %zu bytes for %s operator descriptor",
      sizeof(struct xnn_operator), xnn_operator_type_to_string(operator_type));
    goto error;
  }

  space_to_depth_op->channels = input_channels;
  space_to_depth_op->input_pixel_stride = input_channel_stride;
  space_to_depth_op->output_pixel_stride = output_channel_stride;
  space_to_depth_op->block_size = block_size;

  space_to_depth_op->type = operator_type;
  space_to_depth_op->flags = flags;

  space_to_depth_op->state = xnn_run_state_invalid;

  *space_to_depth_op_out = space_to_depth_op;
  return xnn_status_success;

error:
  xnn_delete_operator(space_to_depth_op);
  return status;
}

enum xnn_status xnn_create_space_to_depth_nhwc_x8(
    size_t input_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    xnn_operator_t* space_to_depth_op_out)
{
  return create_space_to_depth_nhwc(
    input_channels,
    input_channel_stride,
    output_channel_stride,
    block_size,
    flags,
    xnn_operator_type_space_to_depth_nhwc_x8,
    space_to_depth_op_out);
}

enum xnn_status xnn_create_space_to_depth_nhwc_x16(
    size_t input_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    xnn_operator_t* space_to_depth_op_out)
{
  return create_space_to_depth_nhwc(
    input_channels,
    input_channel_stride,
    output_channel_stride,
    block_size,
    flags,
    xnn_operator_type_space_to_depth_nhwc_x16,
    space_to_depth_op_out);
}

enum xnn_status xnn_create_space_to_depth_nhwc_x32(
    size_t input_channels,
    size_t input_channel_stride,
    size_t output_channel_stride,
    uint32_t block_size,
    uint32_t flags,
    xnn_operator_t* space_to_depth_op_out)
{
  return create_space_to_depth_nhwc(
    input_channels,
    input_channel_stride,
    output_channel_stride,
    block_size,
    flags,
    xnn_operator_type_space_to_depth_nhwc_x32,
    space_to_depth_op_out);
}

static enum xnn_status setup_space_to_depth_nhwc(
    xnn_operator_t space_to_depth_op,
    enum xnn_operator_type expected_operator_type,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    uint32_t element_size)
{
  if (space_to_depth_op->type != expected_operator_type) {
    xnn_log_error("failed to setup operator: operator type mismatch (expected %s, got %s)",
      xnn_operator_type_to_string(expected_operator_type),
      xnn_operator_type_to_string(space_to_depth_op->type));
    return xnn_status_invalid_parameter;
  }
  space_to_depth_op->state = xnn_run_state_invalid;

  if ((xnn_params.init_flags & XNN_INIT_FLAG_XNNPACK) == 0) {
    xnn_log_error("failed to setup %s operator: XNNPACK is not initialized",
      xnn_operator_type_to_string(expected_operator_type));
    return xnn_status_uninitialized;
  }

  if (input_width == 0 || input_height == 0) {
    xnn_log_error("failed to setup %s operator with %zux%zu input: input dimensions must be non-zero",
      xnn_operator_type_to_string(expected_operator_type), input_width, input_height);
    return xnn_status_invalid_parameter;
  }

  if (batch_size == 0) {
    space_to_depth_op->state = xnn_run_state_skip;
    return xnn_status_success;
  }

  const uint32_t block_size = space_to_depth_op->block_size;

  const size_t input_shape[5] = {batch_size * (input_height / block_size), block_size, input_width / block_size, block_size, space_to_depth_op->channels};
  const size_t perm[5] = {0, 2, 1, 3, 4};

  const size_t input_stride[5] = {
    block_size * input_width * space_to_depth_op->input_pixel_stride,
    input_width * space_to_depth_op->input_pixel_stride,
    block_size * space_to_depth_op->input_pixel_stride,
    space_to_depth_op->input_pixel_stride,
    1};
  const size_t output_stride[5] = {
    (input_width/block_size) * space_to_depth_op->output_pixel_stride,
    space_to_depth_op->output_pixel_stride,
    block_size * space_to_depth_op->channels,
    space_to_depth_op->channels,
    1};

  return setup_transpose_nd(
      space_to_depth_op,
      input,
      output,
      5,
      input_shape,
      perm,
      input_stride,
      output_stride,
      element_size);
}

enum xnn_status xnn_setup_space_to_depth_nhwc_x8(
    xnn_operator_t space_to_depth_op,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    pthreadpool_t threadpool)
{
  return setup_space_to_depth_nhwc(
    space_to_depth_op,
    xnn_operator_type_space_to_depth_nhwc_x8,
    batch_size, input_height, input_width,
    input, output, sizeof(uint8_t));
}

enum xnn_status xnn_setup_space_to_depth_nhwc_x16(
    xnn_operator_t space_to_depth_op,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    pthreadpool_t threadpool)
{
  return setup_space_to_depth_nhwc(
    space_to_depth_op,
    xnn_operator_type_space_to_depth_nhwc_x16,
    batch_size, input_height, input_width,
    input, output, sizeof(uint16_t));
}

enum xnn_status xnn_setup_space_to_depth_nhwc_x32(
    xnn_operator_t space_to_depth_op,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const void* input,
    void* output,
    pthreadpool_t threadpool)
{
  return setup_space_to_depth_nhwc(
    space_to_depth_op,
    xnn_operator_type_space_to_depth_nhwc_x32,
    batch_size, input_height, input_width,
    input, output, sizeof(uint32_t));
}
