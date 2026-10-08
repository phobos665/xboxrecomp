"""Generated files are cut by weight, at boundaries that do not drift.

chunk_by_weight replaces "N functions a file" as the way the lift cuts its
output (--split-lines, --split-cost). Two properties matter beyond keeping
every function exactly once and in order: files stay inside the size window,
and a boundary depends on the code around it, not on everything before it,
so a one-function change rewrites one or two files and leaves the rest
byte-identical (write_if_changed then skips them, and the build does not
recompile them).
"""

import random
import unittest

from tools.recomp.translator import (chunk_by_weight, compile_cost,
                                     emitted_lines)


def _fake(n, seed=7):
    """n fake translations with a long-tailed spread of sizes."""
    rng = random.Random(seed)
    out = []
    addr = 0x00011000
    for _ in range(n):
        lines = int(rng.paretovariate(1.3) * 20)
        out.append((addr, f"sub_{addr:08X}", "x\n" * lines))
        addr += 0x10 * rng.randint(1, 40)
    return out


def _weight(code):
    return emitted_lines(code)


class ChunkByWeight(unittest.TestCase):

    def test_keeps_every_function_once_in_order(self):
        items = _fake(3000)
        chunks = chunk_by_weight(items, 5000, _weight)
        self.assertEqual([i for c in chunks for i in c], items)

    def test_files_stay_in_the_window(self):
        items = _fake(3000)
        target = 5000
        chunks = chunk_by_weight(items, target, _weight)
        sizes = [sum(_weight(i[2]) for i in c) for c in chunks]
        for k, (chunk, size) in enumerate(zip(chunks, sizes)):
            if len(chunk) > 1:
                self.assertLessEqual(size, target * 5 // 4)
            if k + 1 < len(chunks) and size < target * 3 // 4:
                # Cut short only to keep the next function from overflowing.
                nxt = _weight(chunks[k + 1][0][2])
                self.assertGreater(size + nxt, target * 5 // 4)

    def test_one_function_changed_rewrites_few_files(self):
        """Removing or doubling a function regroups ~1 file, not half of them.

        Measured on real lifts (40 random edits each, Oct 2026): removing
        one function regrouped 1.1-1.2 files on average at --split-cost 14000
        (TimeSplitters 2, 54 files; Shaolin Monks, 86), against 15 and 55
        at --split 250. This fixture's sizes are harsher than a real
        title's, so its bound is looser.
        """
        items = _fake(3000)
        before = chunk_by_weight(items, 5000, _weight)

        def regrouped(changed):
            after = chunk_by_weight(changed, 5000, _weight)
            return len({tuple(i[0] for i in c) for c in after}
                       - {tuple(i[0] for i in c) for c in before})

        rng = random.Random(3)
        counts = []
        for _ in range(30):
            k = rng.randrange(len(items))
            counts.append(regrouped(items[:k] + items[k + 1:]))
            counts.append(regrouped(
                [(a, n, c + c if j == k else c)
                 for j, (a, n, c) in enumerate(items)]))
        # By count, a removal shifts every later file: half of them.
        self.assertLess(sum(counts) / len(counts), 4, counts)
        self.assertLess(max(counts), len(before) // 3, counts)

    def test_cost_counts_labels_and_x87_accesses(self):
        code = ("void f(void)\n{\nloc_00011000: ;\n    fp_push(1.0);\n"
                "loc_00011004: ;\n    fp_pop();\n}\n")
        self.assertEqual(compile_cost(code), 2 * 3 + 2 * 1)


if __name__ == "__main__":
    unittest.main()
