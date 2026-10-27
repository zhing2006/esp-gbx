#!/usr/bin/env python3
"""Rejudge current-protocol SM83 observations without connecting to a board.

Actual interpreter results are rechecked against the suite, then native JIT
results are compared with them. Compact PASS records cannot be rejudged. This
does not create a reusable live interpreter baseline or repair incomplete runs.
"""

from run_sm83 import Sm83
from single_step import rejudge_main


if __name__ == '__main__':
    raise SystemExit(rejudge_main(Sm83(), __doc__, 'Relocated original suite (defaults to the saved path)'))
