"""Listening sessions: the toolkit's commands (scripts/generative/listening/, #403). `listen.py --help` lists them."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from listening.cli import main

if __name__ == "__main__":
    sys.exit(main())
