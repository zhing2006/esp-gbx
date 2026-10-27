#!/usr/bin/env python3
"""Rejudge current-protocol ARM7TDMI observations without connecting to a board.

Saved interpreter actuals are rechecked against the suite. Runs of device
passes have no actual and cannot be rejudged. This does not create a reusable
live interpreter baseline or repair incomplete runs.
"""

from run_arm7 import Arm7
from single_step import rejudge_main


if __name__ == '__main__':
    raise SystemExit(rejudge_main(Arm7(), __doc__, 'Relocated original suite (defaults to the saved path)'))
