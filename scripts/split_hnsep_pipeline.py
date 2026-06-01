#!/usr/bin/env python3
"""Split hnsep_VR_convstft.onnx into pre/core/post submodels.

The split points are chosen so CPU handles waveform preprocessing and waveform
reconstruction, while the main separation network core can be delegated to a
GPU execution provider.
"""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path

import onnx
from onnx import TensorProto, checker, helper, shape_inference


PRE_INPUTS = ["waveform"]
PRE_OUTPUTS = [
    "/Slice_2_output_0",
    "/Concat_7_output_0",
    "/Concat_11_output_0",
    "/Transpose_2_output_0",
    "/Unsqueeze_output_0",
    "/Unsqueeze_13_output_0",
    "/Gather_3_output_0",
]

CORE_INPUTS = [
    "/Slice_2_output_0",
    "/Concat_7_output_0",
    "/Concat_11_output_0",
]
CORE_OUTPUTS = [
    "/Div_5_output_0",
]

POST_INPUTS = [
    "waveform",
    "/Transpose_2_output_0",
    "/Div_5_output_0",
    "/Unsqueeze_output_0",
    "/Unsqueeze_13_output_0",
    "/Gather_3_output_0",
]
POST_OUTPUTS = ["harmonic", "noise"]


def collect_required_node_indices(model: onnx.ModelProto, outputs: list[str], stop_inputs: set[str]) -> set[int]:
    nodes = list(model.graph.node)
    producer: dict[str, int] = {}
    for idx, node in enumerate(nodes):
        for output in node.output:
            producer[output] = idx

    required: set[int] = set()
    queue = deque(outputs)
    while queue:
        value_name = queue.popleft()
        if value_name in stop_inputs:
            continue

        node_index = producer.get(value_name)
        if node_index is None or node_index in required:
            continue

        required.add(node_index)
        for input_name in nodes[node_index].input:
            if input_name:
                queue.append(input_name)

    return required


def build_value_info_lookup(model: onnx.ModelProto) -> dict[str, onnx.ValueInfoProto]:
    lookup: dict[str, onnx.ValueInfoProto] = {}
    for value in list(model.graph.input) + list(model.graph.output) + list(model.graph.value_info):
        lookup[value.name] = value
    return lookup


def make_fallback_value_info(name: str) -> onnx.ValueInfoProto:
    return helper.make_tensor_value_info(name, TensorProto.FLOAT, None)


def clone_value_info(name: str, lookup: dict[str, onnx.ValueInfoProto]) -> onnx.ValueInfoProto:
    value_info = lookup.get(name)
    if value_info is None:
        return make_fallback_value_info(name)
    cloned = onnx.ValueInfoProto()
    cloned.CopyFrom(value_info)
    return cloned


def infer_value_info_map(model: onnx.ModelProto) -> dict[str, onnx.ValueInfoProto]:
    inferred = shape_inference.infer_shapes(model)
    return build_value_info_lookup(inferred)


def build_submodel(
    source: onnx.ModelProto,
    node_indices: set[int],
    inputs: list[str],
    outputs: list[str],
) -> onnx.ModelProto:
    graph = source.graph
    nodes = [graph.node[idx] for idx in sorted(node_indices)]
    used_names = {name for node in nodes for name in [*node.input, *node.output] if name}

    initializer_names = {init.name for init in graph.initializer}
    selected_initializers = [init for init in graph.initializer if init.name in used_names and init.name not in inputs]
    sparse_initializers = [init for init in graph.sparse_initializer if init.values.name in used_names]

    value_lookup = infer_value_info_map(source)
    graph_inputs = [clone_value_info(name, value_lookup) for name in inputs]
    graph_outputs = [clone_value_info(name, value_lookup) for name in outputs]

    subgraph = helper.make_graph(
        nodes=nodes,
        name=graph.name + "_split",
        inputs=graph_inputs,
        outputs=graph_outputs,
        initializer=selected_initializers,
        sparse_initializer=sparse_initializers,
    )

    model = helper.make_model(
        subgraph,
        producer_name=source.producer_name,
        producer_version=source.producer_version,
        domain=source.domain,
        model_version=source.model_version,
        doc_string=source.doc_string,
        opset_imports=list(source.opset_import),
    )
    model.ir_version = source.ir_version
    if source.metadata_props:
        model.metadata_props.extend(source.metadata_props)
    return model


def save_checked(model: onnx.ModelProto, path: Path) -> None:
    inferred = shape_inference.infer_shapes(model)
    checker.check_model(inferred)
    onnx.save(inferred, path)
    print(f"Wrote {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Input hnsep_VR_convstft.onnx")
    parser.add_argument("output_dir", type=Path, help="Output directory for split models")
    args = parser.parse_args()

    model = onnx.load(args.input, load_external_data=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    pre_nodes = collect_required_node_indices(model, PRE_OUTPUTS, set(PRE_INPUTS))
    core_nodes = collect_required_node_indices(model, CORE_OUTPUTS, set(CORE_INPUTS))
    post_nodes = collect_required_node_indices(model, POST_OUTPUTS, set(POST_INPUTS))

    save_checked(build_submodel(model, pre_nodes, PRE_INPUTS, PRE_OUTPUTS), args.output_dir / "hnsep_pre.onnx")
    save_checked(build_submodel(model, core_nodes, CORE_INPUTS, CORE_OUTPUTS), args.output_dir / "hnsep_core.onnx")
    save_checked(build_submodel(model, post_nodes, POST_INPUTS, POST_OUTPUTS), args.output_dir / "hnsep_post.onnx")


if __name__ == "__main__":
    main()
