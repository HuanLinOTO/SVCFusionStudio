#!/usr/bin/env python3
"""Replace the HNSep ONNX STFT node with an equivalent fixed Conv1D subgraph.

The DirectML execution provider is sensitive to ONNX STFT. HNSep uses a single
STFT with a Hann window, hop length 512, and frame length 2048. This script
rewrites that node into widely-supported primitive ONNX operators:

    Unsqueeze([N, L] -> [N, 1, L])
    Conv1D(fixed DFT basis, stride=hop_length)
    Reshape([N, 2 * F, T] -> [N, 2, F, T])
    Transpose([N, 2, F, T] -> [N, T, F, 2])

The resulting model is intended as an experimental DirectML-friendly HNSep
variant. Keep the original model as the CPU fallback.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, checker, helper, numpy_helper


def _constant_tensor_value(node: onnx.NodeProto) -> np.ndarray | None:
    if node.op_type != "Constant":
        return None

    for attr in node.attribute:
        if attr.name == "value":
            return numpy_helper.to_array(attr.t)
        if attr.name == "value_int":
            return np.asarray(attr.i, dtype=np.int64)
        if attr.name == "value_float":
            return np.asarray(attr.f, dtype=np.float32)

    return None


def _collect_constant_values(model: onnx.ModelProto) -> dict[str, np.ndarray]:
    values: dict[str, np.ndarray] = {}

    for initializer in model.graph.initializer:
        values[initializer.name] = numpy_helper.to_array(initializer)

    for node in model.graph.node:
        value = _constant_tensor_value(node)
        if value is not None and len(node.output) == 1:
            values[node.output[0]] = value

    return values


def _get_scalar_int(values: dict[str, np.ndarray], name: str, label: str) -> int:
    value = values.get(name)
    if value is None:
        raise ValueError(f"STFT {label} input {name!r} is not a constant")
    if value.size != 1:
        raise ValueError(f"STFT {label} input {name!r} is not scalar: {value.shape}")
    return int(value.reshape(()))


def _build_dft_conv_weights(window: np.ndarray, frame_length: int) -> np.ndarray:
    if window.ndim != 1:
        raise ValueError(f"STFT window must be 1D, got {window.shape}")
    if window.shape[0] != frame_length:
        raise ValueError(
            f"STFT window length {window.shape[0]} does not match frame length {frame_length}"
        )
    if frame_length <= 0 or frame_length & (frame_length - 1):
        raise ValueError(f"STFT frame length must be a power of two, got {frame_length}")

    bins = frame_length // 2 + 1
    sample = np.arange(frame_length, dtype=np.float32)
    window = window.astype(np.float32, copy=False)

    weights = np.empty((bins * 2, 1, frame_length), dtype=np.float32)
    for bin_index in range(bins):
        phase = (2.0 * math.pi * float(bin_index) / float(frame_length)) * sample
        weights[bin_index, 0, :] = window * np.cos(phase)
        weights[bins + bin_index, 0, :] = window * -np.sin(phase)

    return weights


def replace_stft_with_conv(model: onnx.ModelProto) -> onnx.ModelProto:
    stft_nodes = [node for node in model.graph.node if node.op_type == "STFT"]
    if len(stft_nodes) != 1:
        raise ValueError(f"Expected exactly one STFT node, found {len(stft_nodes)}")

    stft = stft_nodes[0]
    if len(stft.input) < 4:
        raise ValueError("Expected STFT inputs: signal, frame_step, window, frame_length")
    if len(stft.output) != 1:
        raise ValueError("Expected STFT to have exactly one output")

    onesided = 0
    for attr in stft.attribute:
        if attr.name == "onesided":
            onesided = int(attr.i)
    if onesided != 1:
        raise ValueError(f"Only onesided STFT is supported, got onesided={onesided}")

    values = _collect_constant_values(model)
    signal_name, frame_step_name, window_name, frame_length_name = stft.input[:4]
    hop_length = _get_scalar_int(values, frame_step_name, "frame_step")
    frame_length = _get_scalar_int(values, frame_length_name, "frame_length")

    window = values.get(window_name)
    if window is None:
        raise ValueError(f"STFT window input {window_name!r} is not a constant")

    weights = _build_dft_conv_weights(window, frame_length)
    bins = frame_length // 2 + 1

    prefix = "HNSepConvSTFT"
    unsqueezed_signal = f"/{prefix}/UnsqueezeSignal_output_0"
    conv_output = f"/{prefix}/Conv_output_0"
    reshaped_output = f"/{prefix}/Reshape_output_0"
    replacement_output = f"/{prefix}/TransposeToStftLayout_output_0"
    weight_name = f"/{prefix}/weight"
    shape_name = f"/{prefix}/shape"
    axes_name = f"/{prefix}/unsqueeze_axes"

    model.graph.initializer.extend(
        [
            numpy_helper.from_array(weights, name=weight_name),
            numpy_helper.from_array(np.asarray([0, 2, bins, -1], dtype=np.int64), name=shape_name),
            numpy_helper.from_array(np.asarray([1], dtype=np.int64), name=axes_name),
        ]
    )

    replacement_nodes = [
        helper.make_node(
            "Unsqueeze",
            [signal_name, axes_name],
            [unsqueezed_signal],
            name=f"/{prefix}/UnsqueezeSignal",
        ),
        helper.make_node(
            "Conv",
            [unsqueezed_signal, weight_name],
            [conv_output],
            name=f"/{prefix}/Conv",
            strides=[hop_length],
        ),
        helper.make_node(
            "Reshape",
            [conv_output, shape_name],
            [reshaped_output],
            name=f"/{prefix}/Reshape",
        ),
        helper.make_node(
            "Transpose",
            [reshaped_output],
            [replacement_output],
            name=f"/{prefix}/TransposeToStftLayout",
            perm=[0, 3, 2, 1],
        ),
    ]

    original_output = stft.output[0]
    for node in model.graph.node:
        for input_index, input_name in enumerate(node.input):
            if input_name == original_output:
                node.input[input_index] = replacement_output

    rewritten_nodes = []
    for node in model.graph.node:
        if node is stft:
            rewritten_nodes.extend(replacement_nodes)
        else:
            rewritten_nodes.append(node)

    del model.graph.node[:]
    model.graph.node.extend(rewritten_nodes)

    return model


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Input hnsep_VR.onnx path")
    parser.add_argument("output", type=Path, help="Output rewritten ONNX path")
    parser.add_argument(
        "--skip-check",
        action="store_true",
        help="Skip onnx.checker.check_model. Useful if the local onnx package crashes.",
    )
    args = parser.parse_args()

    print(f"Loading {args.input}", flush=True)
    model = onnx.load(args.input, load_external_data=True)
    print("Replacing STFT node", flush=True)
    model = replace_stft_with_conv(model)

    if not args.skip_check:
        print("Checking rewritten model", flush=True)
        checker.check_model(model)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    print(f"Saving {args.output}", flush=True)
    onnx.save(model, args.output)

    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
