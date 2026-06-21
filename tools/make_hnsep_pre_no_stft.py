import argparse
import copy
import tempfile
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, checker, helper, numpy_helper


CONV_NODE_NAME = "/HNSepConvSTFT/Conv"


def find_node(graph, name):
    for node in graph.node:
        if node.name == name:
            return node
    raise RuntimeError(f"node not found: {name}")


def add_graph_input_if_missing(graph, name):
    if any(value.name == name for value in graph.input):
        return

    value_info = helper.make_tensor_value_info(
        name,
        TensorProto.FLOAT,
        ["batch_size", 2050, "stft_frames"],
    )
    graph.input.append(value_info)


def remove_node(graph, node_to_remove):
    kept = [node for node in graph.node if node is not node_to_remove]
    del graph.node[:]
    graph.node.extend(kept)


def strip_initializer(graph, name):
    kept = [init for init in graph.initializer if init.name != name]
    del graph.initializer[:]
    graph.initializer.extend(kept)


def make_pre_no_stft(input_path, output_path):
    model = onnx.load(input_path)
    graph = model.graph
    conv = find_node(graph, CONV_NODE_NAME)
    if len(conv.output) != 1:
        raise RuntimeError(f"unexpected ConvSTFT output count: {len(conv.output)}")

    conv_output_name = conv.output[0]
    add_graph_input_if_missing(graph, conv_output_name)

    if len(conv.input) > 1:
        strip_initializer(graph, conv.input[1])
    remove_node(graph, conv)

    checker.check_model(model)

    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = Path(tmp_dir) / "hnsep_pre_no_stft_unpruned.onnx"
        onnx.save(model, tmp_path)
        output_names = [output.name for output in graph.output]
        onnx.utils.extract_model(
            str(tmp_path),
            str(output_path),
            ["waveform", conv_output_name],
            output_names,
        )

    checker.check_model(onnx.load(output_path))
    return conv_output_name


def make_reference_with_conv_output(input_path, output_path):
    model = onnx.load(input_path)
    graph = model.graph
    conv = find_node(graph, CONV_NODE_NAME)
    conv_output_name = conv.output[0]
    if not any(output.name == conv_output_name for output in graph.output):
        graph.output.append(
            helper.make_tensor_value_info(
                conv_output_name,
                TensorProto.FLOAT,
                ["batch_size", 2050, "stft_frames"],
            )
        )
    checker.check_model(model)
    onnx.save(model, output_path)
    return conv_output_name


def verify_equivalence(input_path, output_path, conv_output_name, seconds):
    import onnxruntime as ort

    ref_path = Path(output_path).with_name("hnsep_pre_ref_with_conv_output.onnx")
    make_reference_with_conv_output(input_path, ref_path)

    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    options.intra_op_num_threads = 4

    ref = ort.InferenceSession(str(ref_path), options, providers=["CPUExecutionProvider"])
    mod = ort.InferenceSession(str(output_path), options, providers=["CPUExecutionProvider"])

    rng = np.random.default_rng(1234)
    audio = rng.standard_normal((1, int(44100 * seconds)), dtype=np.float32) * 0.03
    ref_outputs = ref.run(None, {"waveform": audio})
    ref_output_names = [output.name for output in ref.get_outputs()]
    conv_index = ref_output_names.index(conv_output_name)
    conv_output = ref_outputs[conv_index]

    expected = {
        name: value
        for name, value in zip(ref_output_names, ref_outputs)
        if name != conv_output_name
    }

    mod_inputs = {}
    for input_meta in mod.get_inputs():
        if input_meta.name == "waveform":
            mod_inputs[input_meta.name] = audio
        elif input_meta.name == conv_output_name:
            mod_inputs[input_meta.name] = conv_output
        else:
            raise RuntimeError(f"unexpected modified model input: {input_meta.name}")

    actual_outputs = mod.run(None, mod_inputs)
    actual_names = [output.name for output in mod.get_outputs()]

    max_abs = 0.0
    for name, actual in zip(actual_names, actual_outputs):
        diff = np.max(np.abs(actual - expected[name]))
        max_abs = max(max_abs, float(diff))
        print(f"{name}: max_abs_diff={diff:.6g}, shape={actual.shape}")

    ref_path.unlink(missing_ok=True)
    return max_abs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--verify-seconds", type=float, default=1.0)
    args = parser.parse_args()

    conv_output_name = make_pre_no_stft(args.input, args.output)
    print(f"created {args.output}")
    print(f"stft input name: {conv_output_name}")

    if args.verify:
        max_abs = verify_equivalence(
            args.input, args.output, conv_output_name, args.verify_seconds
        )
        print(f"verification max_abs_diff={max_abs:.6g}")


if __name__ == "__main__":
    main()
