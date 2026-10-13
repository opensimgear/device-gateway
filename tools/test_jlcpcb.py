import tempfile
import unittest
from pathlib import Path

from jlcpcb import assembly_rows, select_version


def part(ref, value="10k", mpn="MPN-A", footprint="KiCad:R_0805", catalog=""):
    return dict(Reference=ref, Value=value, MPN=mpn, Footprint=footprint, LCSC=catalog)


def placement(ref, side="top", rotation="0", value="10k", package="R_0805"):
    return dict(Ref=ref, Val=value, Package=package, PosX="10.25", PosY="-20.5",
                Rot=rotation, Side=side)


class JLCPCBTests(unittest.TestCase):
    def test_latest_is_numeric_and_explicit_version_is_checked(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)
            root = repo / "electronics/boards/gateway"
            for version in ("0.9.0", "0.10.0", "0.10.1", "home-milling", "1.0"):
                (root / version).mkdir(parents=True)
            self.assertEqual(select_version(repo, "gateway").name, "0.10.1")
            self.assertEqual(select_version(repo, "gateway", "0.9.0").name, "0.9.0")
            with self.assertRaises(ValueError):
                select_version(repo, "gateway", "../../other")

    def test_group_exact_parts_and_keep_bom_cpl_references_equal(self):
        bom = [part("R1"), part("R2"), part("R3", mpn="MPN-B"),
               part("H1", footprint="KiCad:MountingHole_3.2mm_M3", mpn="")]
        pos = [placement("R1"), placement("R2", "bottom", "-90"), placement("R3"),
               placement("H1"), placement("EXCLUDED")]
        rows, cpl, missing = assembly_rows(bom, pos)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0][1], "R1,R2")
        self.assertEqual(rows[0][-1], 2)
        self.assertEqual(cpl[1], ["R2", "10.250000mm", "-20.500000mm", "Bottom", "270.000000"])
        self.assertEqual({r for row in rows for r in row[1].split(",")}, {p[0] for p in cpl})
        self.assertEqual(missing, ["R1", "R2", "R3"])

    def test_catalog_number_and_missing_mpn(self):
        rows, _, missing = assembly_rows([part("R1", catalog="C123")], [placement("R1")])
        self.assertEqual(rows[0][4], "C123")
        self.assertFalse(missing)
        for bad in (part("R1", mpn=""), part("R1", catalog="unknown")):
            with self.assertRaises(ValueError):
                assembly_rows([bad], [placement("R1")])

    def test_mismatch_duplicate_and_invalid_coordinates_fail(self):
        for positions in ([placement("R1", value="1k")],
                          [placement("R1"), placement("R1")],
                          [dict(placement("R1"), PosX="nan")]):
            with self.assertRaises(ValueError):
                assembly_rows([part("R1")], positions)

    def test_missing_placement_fails_but_smd_subset_is_allowed(self):
        with self.assertRaises(ValueError):
            assembly_rows([part("R1"), part("R2")], [placement("R1")])
        rows, cpl, _ = assembly_rows([part("R1"), part("R2")], [placement("R1")], require_all=False)
        self.assertEqual(len(cpl), 1)


if __name__ == "__main__":
    unittest.main()
