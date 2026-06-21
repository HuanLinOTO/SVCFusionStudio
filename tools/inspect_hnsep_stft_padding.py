import argparse
from pathlib import Path

import numpy as np
import onnx


def constant_arrays(model):
    arrays = {}
    for init in model.graph.initializer:
        arrays[init.name] = onnx.numpy_helper.to_array(init)
    for node in model.graph.node:
        if node.op_type == "Constant" and node.output:
            for attr in node.attribute:
                if attr.name == "value":
                    arrays[node.output[0]] = onnx.numpy_helper.to_array(attr.t)
    return arrays


def print_first_nodes(model, count):
    arrays = constant_arrays(model)
    for index, node in enumerate(model.graph.node[:count]):
        print(f"[{index:02d}] {node.op_type} {node.name}")
        print(f"  inputs: {list(node.input)}")
        print(f"  outputs: {list(node.output)}")
        for input_name in node.input:
            if input_name in arrays:
                value = arrays[input_name]
                print(f"  const {input_name}: shape={value.shape}, value={value}")
        for attr in node.attribute:
            if attr.type == onnx.AttributeProto.INTS:
                print(f"  attr {attr.name}: {list(attr.ints)}")
            elif attr.type == onnx.AttributeProto.INT:
                print(f"  attr {attr.name}: {attr.i}")
            elif attr.type == onnx.AttributeProto.STRING:
                print(f"  attr {attr.name}: {attr.s!r}")


def probe_conv_frames(model_path, seconds_list):
    import onnxruntime as ort

    model = onnx.load(model_path)
    conv = next(node for node in model.graph.node if node.name == "/HNSepConvSTFT/Conv")
    conv_output = conv.output[0]
    if not any(output.name == conv_output for output in model.graph.output):
        model.graph.output.append(
            onnx.helper.make_tensor_value_info(
                conv_output,
                onnx.TensorProto.FLOAT,
                ["batch_size", 2050, "stft_frames"],
            )
        )
    tmp_path = Path(model_path).with_name("hnsep_pre_probe_conv.onnx")
    onnx.save(model, tmp_path)

    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    options.intra_op_num_threads = 4
    session = ort.InferenceSession(str(tmp_path), options, providers=["CPUExecutionProvider"])
    names = [output.name for output in session.get_outputs()]
    conv_index = names.index(conv_output)
    rng = np.random.default_rng(123)
    for seconds in seconds_list:
        samples = int(round(44100 * seconds))
        audio = rng.standard_normal((1, samples), dtype=np.float32) * 0.03
        conv_value = session.run(None, {"waveform": audio})[conv_index]
        frames = conv_value.shape[-1]
        padded_length = (frames - 1) * 512 + 2048
        print(
            f"samples={samples}, frames={frames}, inferred_padded_length={padded_length}, "
            f"total_pad={padded_length - samples}"
        )
    tmp_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model")
    parser.add_argument("--nodes", type=int, default=45)
    parser.add_argument("--probe", action="store_true")
    args = parser.parse_args()

    model = onnx.load(args.model)
    print_first_nodes(model, args.nodes)
    if args.probe:
        probe_conv_frames(args.model, [0.1, 0.5, 1.0, 3.0, 7.2584, 30.0])


if __name__ == "__main__":
    main()
