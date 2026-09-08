"""XBE (Xbox Executable) parsing.

Stage 1 of the pipeline. Reads the XBE header, section table, library
versions, certificate and kernel imports for any original Xbox title.

    python3 -m tools.xbe_parser game_files/default.xbe --json game_files/g_analysis.json

The --json output is required by tools.disasm, which reads the section
layout from it. Keep it beside the XBE.
"""

from .xbe_parser import XBEParser, XBEHeader, XBESectionHeader

__all__ = ["XBEParser", "XBEHeader", "XBESectionHeader"]
