"""--data-sections: a title's own data sections are not swept as code.

    python3 -m unittest tools.disasm.test_data_sections

An XBE marks nearly every section executable. BLiNX links 42 model and map
sections into its image, and swept as code they decoded into 10,700 phantom
functions. config/sections/<TITLEID>.json names them; load_image must then
load them as non-executable, so every pass that asks `section.executable`
turns them away -- and must leave everything else alone.
"""

import json
import os
import tempfile
import unittest

from .loader import is_data_section, load_image


def _section(name, va, executable=True):
    return {"name": name, "virtual_addr": "0x%08X" % va, "virtual_size": 0x100,
            "raw_addr": "0x00001000", "raw_size": 0x100, "writable": True,
            "executable": executable, "flags": "X"}


class DataSectionTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.xbe = os.path.join(self.dir.name, "default.xbe")
        with open(self.xbe, "wb") as f:
            f.write(b"XBEH" + bytes(0x2000))
        self.json = os.path.join(self.dir.name, "default_analysis.json")
        with open(self.json, "w") as f:
            json.dump({
                "base_address": "0x00010000", "image_size": 0x100000,
                "entry_point": "0x00012000", "kernel_thunk_addr": "0x00011000",
                "kernel_imports": [],
                "sections": [_section(".text", 0x12000),
                             _section("D3D", 0x13000),
                             _section("MDLPL", 0x14000),
                             _section("MAP13", 0x15000),
                             _section("MAPPER", 0x16000),
                             _section(".data", 0x17000)],
            }, f)

    def tearDown(self):
        self.dir.cleanup()

    def _code(self, patterns):
        image = load_image(self.xbe, self.json, data_sections=patterns)
        return [s.name for s in image.get_code_sections()]

    def test_without_patterns_only_the_pe_names_are_data(self):
        self.assertEqual(self._code(None),
                         [".text", "D3D", "MDLPL", "MAP13", "MAPPER"])

    def test_patterns_take_matching_sections_out_of_the_sweep(self):
        self.assertEqual(self._code(["MDL*", "MAP[0-9]*"]),
                         [".text", "D3D", "MAPPER"])

    def test_a_matched_section_is_not_executable_for_any_pass(self):
        image = load_image(self.xbe, self.json, data_sections=["MDL*"])
        self.assertFalse(image.get_section("MDLPL").executable)
        self.assertTrue(image.get_section("D3D").executable)

    def test_matching_is_case_sensitive(self):
        self.assertFalse(is_data_section("mdlpl", ["MDL*"]))
        self.assertTrue(is_data_section(".rdata"))


if __name__ == "__main__":
    unittest.main()
