# SPDX-License-Identifier: Apache-2.0 OR MIT
# Console criteria for the pico-examples programs that need an external
# part, kept beside the parts they attach (parts.py) as each model lands.
# Same rules as targets.py: every pattern comes from the program's source.

from test.pico_examples.targets import Spec

PART_SPECS: dict[str, Spec] = {}
