#!/usr/bin/env python3
# Copyright (C) 2026 BlissLabs
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import glob

HEADER = """// Copyright (C) 2026 BlissLabs
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
"""

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    iptsd_root = os.path.dirname(script_dir)
    etc_dir = os.path.join(iptsd_root, "etc")
    presets_dir = os.path.join(etc_dir, "presets")

    output_bp = os.path.join(etc_dir, "Android.bp")

    preset_files = sorted(glob.glob(os.path.join(presets_dir, "*.conf")))
    preset_names = [os.path.basename(p) for p in preset_files]

    all_modules = ["iptsd.conf"] + preset_names

    with open(output_bp, "w") as f:
        f.write(HEADER)
        f.write("\n")
        f.write("prebuilt_etc {\n")
        f.write('    name: "iptsd.conf",\n')
        f.write('    src: "iptsd.conf",\n')
        f.write("    proprietary: true,\n")
        f.write("}\n")

        for name in preset_names:
            f.write("\nprebuilt_etc {\n")
            f.write(f'    name: "{name}",\n')
            f.write(f'    src: "presets/{name}",\n')
            f.write('    sub_dir: "ipts",\n')
            f.write("    proprietary: true,\n")
            f.write("}\n")

        f.write("\nphony {\n")
        f.write('    name: "iptsd-presets",\n')
        f.write("    required: [\n")
        for mod in all_modules:
            f.write(f'        "{mod}",\n')
        f.write("    ],\n")
        f.write("}\n")

    print(f"Successfully generated {output_bp}")

if __name__ == "__main__":
    main()
