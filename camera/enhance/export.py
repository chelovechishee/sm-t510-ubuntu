#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Export the LUTwithBGrid predictor for t510-camera's "Enhance".

LUTwithBGrid (Kim and Cho, ECCV 2024; Apache-2.0):
https://github.com/WontaeaeKim/LUTwithBGrid. The predictor is the authors'
model with their FiveK_sRGB weights. Their slicing kernels (CPU and CUDA
alike) compute the pixel position with an integer division,
x_ / (width - 1), so every pixel but the last row and column samples the
grids at x = y = 0: the trained model is a global colour transform, six 1D
curves (two per channel), a 1x1 mixing layer and a 3D LUT (tetrahedral).
Only the predictor goes to ONNX; tools/camera-enhance.c does the rest.

usage: export.py LUTwithBGrid/pretrained/FiveK_sRGB.pth out.onnx
Needs: torch (CPU), onnx.
"""
import sys

import numpy as np
import torch
import torch.nn as nn

NV = 17  # vertices of the LUT and grids


def block(i, o, norm):
    layers = [nn.Conv2d(i, o, 3, stride=2, padding=1), nn.LeakyReLU(0.2)]
    if norm:
        layers.append(nn.InstanceNorm2d(o, affine=True))
    return layers


class Backbone(nn.Module):
    def __init__(self, c=8):
        super().__init__()
        self.c = c
        self.model = nn.Sequential(
            nn.Upsample(size=(256, 256), mode='bilinear'),
            nn.Conv2d(3, c, 3, stride=2, padding=1), nn.LeakyReLU(0.2),
            nn.InstanceNorm2d(c, affine=True),
            *block(c, 2 * c, True), *block(2 * c, 4 * c, True),
            *block(4 * c, 8 * c, True), *block(8 * c, 8 * c, False),
            nn.Dropout(p=0.5), nn.AvgPool2d(5, stride=2))

    def forward(self, x):
        return self.model(x).view([-1, self.c * 32])


class Gen(nn.Module):
    def __init__(self, n_out, n_feats=256, n_ranks=8):
        super().__init__()
        self.weights_generator = nn.Linear(n_feats, n_ranks)
        self.bank = nn.Linear(n_ranks, n_out * NV ** 3)
        self.n_out = n_out

    def forward(self, f):
        return self.bank(self.weights_generator(f)).view([-1, self.n_out, NV, NV, NV])


class Predictor(nn.Module):
    """image (1,3,h,w) in 0..1 -> lut (1,3,17,17,17), curves (1,6,17)"""

    def __init__(self):
        super().__init__()
        self.backbone = Backbone()
        self.gen_3d_lut = Gen(3)
        self.gen_bilateral = Gen(6)

    def forward(self, x):
        f = self.backbone(x)
        lut = self.gen_3d_lut(f)
        grids = self.gen_bilateral(f)
        # grid[c][value][y][x] at x = y = 0
        return lut, grids[:, :, :, 0, 0]


def load(path):
    sd = torch.load(path, map_location='cpu')
    p = Predictor()
    own = {}
    for k, v in sd.items():
        k2 = k.replace('basis_luts_bank', 'bank').replace('basis_grids_bank', 'bank')
        if not k2.startswith('slice.'):
            own[k2] = v
    p.load_state_dict(own)
    p.eval()
    mix_w = sd['slice.conv_1d.weight'][:, :, 0, 0].numpy()  # (3, 9)
    mix_b = sd['slice.conv_1d.bias'].numpy()
    return p, mix_w, mix_b


def main():
    import onnx
    weights, out = sys.argv[1:3]
    p, w, b = load(weights)
    torch.onnx.export(p, torch.rand(1, 3, 406, 541), out, opset_version=17,
                      input_names=["image"], output_names=["lut", "curves"],
                      dynamic_axes={"image": {2: "height", 3: "width"}},
                      dynamo=False)
    m = onnx.load(out)
    # ONNX Runtime 1.23 (Ubuntu 26.04) reads IR 9 at most
    m.ir_version = min(m.ir_version, 9)
    m.metadata_props.add(key="mix_weight", value=",".join("%.9g" % v for v in w.ravel()))
    m.metadata_props.add(key="mix_bias", value=",".join("%.9g" % v for v in b))
    m.metadata_props.add(key="source", value="LUTwithBGrid (Kim & Cho, ECCV 2024), FiveK_sRGB.pth, Apache-2.0")
    onnx.checker.check_model(m)
    onnx.save(m, out)


if __name__ == "__main__":
    main()
