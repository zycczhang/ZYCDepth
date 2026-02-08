#!/usr/bin/env python3
"""
Export Depth Anything 3 to ONNX (fixed square resolution).

Example:
    python export_onnx.py \
        --model depth-anything/DA3-SMALL \
        --process-res 504 \
        --output DA3-SMALL-504.onnx
"""

import argparse
import os
import sys
from pathlib import Path

import torch
from torch.onnx import symbolic_helper as sym_help
# --- 核心修复：注册 aten::mT 的导出逻辑 ---
def mT_symbolic(g, self):
    # mT 本质上是转置最后两个维度
    # 在 ONNX 中，这对应于 Transpose 算子
    # 由于该模型中 intrinsics 等张量通常是 4 维 [B, 1, 3, 3]
    # 我们假设通用的转置逻辑
    return g.op("Transpose", self, perm_i=[0, 1, 3, 2])
# 注册到 aten 命名空间
from torch.onnx import register_custom_op_symbolic
try:
    # 针对 opset 17 注册
    register_custom_op_symbolic('aten::mT', mT_symbolic, 17)
    print("Successfully registered symbolic for aten::mT")
except Exception as e:
    print(f"Failed to register symbolic: {e}")
# ----------------------------------------
# Ensure local package import works when running from repo root
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "src"))

from depth_anything_3.api import DepthAnything3


class OnnxWrapper(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model.model
        self.model.eval()

    def forward(self, image):
        # image: (1, 3, H, W) -> (B=1, N=1, 3, H, W)
        x = image.unsqueeze(0)

        # --- 修改 1: 开启 infer_gs=True 以推理相机参数 ---
        out = self.model(x, None, None, export_feat_layers=[], infer_gs=True)

        # --- 修改 2: 同时返回深度、内参和外参 ---
        # out["depth"]: [1, 1, H, W] -> [1, H, W]
        # out["intrinsics"]: [1, 1, 3, 3] -> [3, 3]
        # out["extrinsics"]: [1, 1, 3, 4] -> [3, 4]
        depth = out["depth"].squeeze(0)
        intrinsics = out["intrinsics"].squeeze()
        extrinsics = out["extrinsics"].squeeze()

        return depth, intrinsics, extrinsics


def parse_args():
    parser = argparse.ArgumentParser(description="Export Depth Anything 3 to ONNX")
    parser.add_argument(
        "--model",
        type=str,
        default="depth-anything/DA3-SMALL",
        help="Model name or path",
    )
    parser.add_argument(
        "--process-res",
        type=int,
        default=504,
        help="Fixed square resolution (matches run_depth_inference.py)",
    )
    parser.add_argument(
        "--output",
        type=str,
        default="DA3-SMALL-504.onnx",
        help="Output ONNX path",
    )
    parser.add_argument(
        "--opset",
        type=int,
        default=17,  # 适配新版本PyTorch的推荐opset版本
        help="ONNX opset version",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    # 设置PyTorch导出环境
    torch.set_grad_enabled(False)
    device = torch.device("cpu")

    print("Loading PyTorch model: {}".format(args.model))
    pt_model = DepthAnything3.from_pretrained(args.model)
    pt_model = pt_model.to(device)
    pt_model.eval()  # 明确设置eval模式

    wrapper = OnnxWrapper(pt_model)
    wrapper.eval()  # 确保wrapper也处于eval模式

    # 创建固定形状的dummy input
    dummy_input = torch.randn(1, 3, args.process_res, args.process_res, device=device)

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    print("Exporting to ONNX with intrinsics...")
    torch.onnx.export(
        wrapper,
        dummy_input,
        args.output,
        input_names=["image"],
        output_names=["depth", "intrinsics", "extrinsics"],  # 增加输出名
        opset_version=args.opset,
        do_constant_folding=True,
        training=torch.onnx.TrainingMode.EVAL,
        export_params=True,
    )
    print("Saved ONNX model to {}".format(args.output))


if __name__ == "__main__":
    main()