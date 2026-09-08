"""CLI entry point for the XBE parser.

Usage:
    python3 -m tools.xbe_parser game_files/default.xbe --json game_files/g_analysis.json
    python3 -m tools.xbe_parser game_files/default.xbe --extract-sections out/
"""

from .xbe_parser import main

if __name__ == "__main__":
    main()
