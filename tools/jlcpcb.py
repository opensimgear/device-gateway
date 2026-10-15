#!/usr/bin/env python3
"""Generate a JLCPCB Gerber ZIP and matching assembly CSVs from saved KiCad CAD."""
import argparse
import csv
import math
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections import defaultdict
from pathlib import Path

VERSION = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)")
LAYERS = "F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,Edge.Cuts"
SUFFIXES = "F_Cu.gtl In1_Cu.g1 In2_Cu.g2 B_Cu.gbl F_Mask.gts B_Mask.gbs F_Paste.gtp B_Paste.gbp F_Silkscreen.gto B_Silkscreen.gbo Edge_Cuts.gm1 PTH.drl NPTH.drl".split()


def select_version(repo, board, version=None):
    directory = repo / "electronics/boards" / board
    versions = [p.name for p in directory.iterdir()
                if p.is_dir() and VERSION.fullmatch(p.name)]
    if not versions:
        raise ValueError(f"No numbered board versions in {directory}")
    if version is None:
        version = max(versions, key=lambda v: tuple(map(int, v.split("."))))
    if not VERSION.fullmatch(version) or version not in versions:
        raise ValueError(f"Unknown version {version!r}; available: {', '.join(sorted(versions))}")
    return directory / version


def read_csv(path):
    with path.open(newline="", encoding="utf-8-sig") as source:
        return list(csv.DictReader(source))


def write_csv(path, headers, rows):
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(headers)
        writer.writerows(rows)


def assembly_rows(bom, positions, require_all=True):
    """Use the same set of real, populated components in both upload files."""
    components = {}
    for part in bom:
        ref = part["Reference"]
        if ref in components:
            raise ValueError(f"Duplicate BOM reference: {ref}")
        # Mounting holes are board mechanics, even when schematic BOM flags are unset.
        if part["Footprint"].split(":")[-1].startswith("MountingHole"):
            continue
        components[ref] = part
    groups = defaultdict(list)
    cpl = []
    seen = set()
    missing_catalog = []
    for placement in positions:
        ref = placement["Ref"]
        if ref in seen:
            raise ValueError(f"Duplicate placement reference: {ref}")
        seen.add(ref)
        if ref not in components:
            continue  # Mechanics, schematic DNP, or excluded-from-BOM components.
        part = components[ref]
        package = part["Footprint"].split(":")[-1]
        if placement["Package"] != package or placement["Val"] != part["Value"]:
            raise ValueError(f"Schematic/PCB value or footprint mismatch for {ref}")
        mpn = part["MPN"].strip()
        if not mpn:
            raise ValueError(f"Missing manufacturer part number for {ref}")
        catalog = (part.get("LCSC") or part.get("JLCPCB") or "").strip()
        if catalog and not re.fullmatch(r"C\d+", catalog):
            raise ValueError(f"Invalid JLC/LCSC catalog number for {ref}: {catalog!r}")
        if not catalog:
            missing_catalog.append(ref)
        x, y, rotation = (float(placement[k]) for k in ("PosX", "PosY", "Rot"))
        if not all(math.isfinite(n) for n in (x, y, rotation)):
            raise ValueError(f"Non-finite placement coordinates for {ref}")
        layer = {"top": "Top", "bottom": "Bottom"}.get(placement["Side"])
        if layer is None:
            raise ValueError(f"Invalid placement side for {ref}")
        cpl.append([ref, f"{x:.6f}mm", f"{y:.6f}mm", layer, f"{rotation % 360:.6f}"])
        groups[(part["Value"], package, mpn, catalog)].append(ref)
    absent = set(components) - seen
    if require_all and absent:
        raise ValueError(f"BOM components have no populated PCB placement: {', '.join(sorted(absent))}; reconcile schematic/PCB exclusions")
    if not cpl:
        raise ValueError("No populated components selected for assembly")
    rows = [[value, ",".join(refs), footprint, mpn, catalog, len(refs)]
            for (value, footprint, mpn, catalog), refs in groups.items()]
    return rows, cpl, missing_catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", nargs="?", help="MAJOR.MINOR.PATCH; defaults to highest numeric version")
    parser.add_argument("--board", required=True, choices=("gateway", "starter"))
    parser.add_argument("--output-dir", type=Path, help="Output directory; defaults to workspace .research/jlcpcb")
    parser.add_argument("--smd-only", action="store_true", help="Only assemble footprints marked SMD")
    parser.add_argument("--kicad-cli", default="kicad-cli", help="KiCad 10 CLI executable or path")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    try:
        directory = select_version(repo, args.board, args.version)
        for extension in ("kicad_pcb", "kicad_sch"):
            if not (directory / f"{args.board}.{extension}").is_file():
                raise ValueError(f"Missing {args.board}.{extension} in {directory}")
        notices = [repo / "LICENSE", repo / "THIRD_PARTY_NOTICES.md",
                   directory / "LICENSE-KICAD.md"]
        for notice in notices:
            if not notice.is_file():
                raise ValueError(f"Missing distribution notice: {notice}")
        output = (args.output_dir or repo.parent / ".research/jlcpcb").resolve()
        # Keep generated files out of the maintained board sources.
        if output == directory or directory in output.parents:
            raise ValueError("Output directory must be outside the native board directory")
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".jlcpcb-", dir=output) as scratch:
            temp = Path(scratch)
            def run(*arguments):
                subprocess.run([args.kicad_cli, *map(str, arguments)], check=True,
                               stdout=subprocess.PIPE, text=True)
            pcb = directory / f"{args.board}.kicad_pcb"
            sch = directory / f"{args.board}.kicad_sch"
            run("pcb", "export", "gerbers", pcb, "-o", str(temp) + "/",
                "--layers", LAYERS, "--subtract-soldermask", "--no-x2")
            run("pcb", "export", "drill", pcb, "-o", str(temp) + "/",
                "--format", "excellon", "--excellon-units", "mm", "--excellon-separate-th")
            run("sch", "export", "bom", sch, "-o", temp / "native-bom.csv",
                "--fields", "Reference,Value,Footprint,MPN,LCSC,JLCPCB",
                "--labels", "Reference,Value,Footprint,MPN,LCSC,JLCPCB",
                "--exclude-dnp", "--group-by", "", "--ref-range-delimiter", "")
            options = ["--smd-only"] if args.smd_only else []
            run("pcb", "export", "pos", pcb, "-o", temp / "native-positions.csv",
                "--format", "csv", "--units", "mm", "--side", "both", "--exclude-dnp", *options)
            bom, cpl, missing_catalog = assembly_rows(
                read_csv(temp / "native-bom.csv"), read_csv(temp / "native-positions.csv"),
                require_all=not args.smd_only)
            stem = f"{args.board}-{directory.name}"
            if args.smd_only:
                stem += "-smd"
            archive = temp / f"{stem}-gerbers.zip"
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
                for suffix in SUFFIXES:
                    file = temp / f"{args.board}-{suffix}"
                    if not file.is_file() or not file.stat().st_size:
                        raise ValueError(f"Missing or empty fabrication export: {file.name}")
                    bundle.write(file, file.name)
                for notice in notices:
                    bundle.write(notice, notice.name)
            bom_path = temp / f"{stem}-bom.csv"
            cpl_path = temp / f"{stem}-cpl.csv"
            write_csv(bom_path, ["Comment", "Designator", "Footprint", "Manufacturer Part Number",
                                 "JLCPCB Part #", "Quantity"], bom)
            write_csv(cpl_path, ["Designator", "Mid X", "Mid Y", "Layer", "Rotation"], cpl)
            notice_paths = [temp / f"{stem}-{notice.name}" for notice in notices]
            for notice, staged in zip(notices, notice_paths):
                shutil.copy2(notice, staged)
            for file in (archive, bom_path, cpl_path, *notice_paths):
                shutil.move(str(file), str(output / file.name))
                print(f"Created: {output / file.name}")
            print(f"Assembly: {len(cpl)} components in {len(bom)} BOM groups.")
            if missing_catalog:
                print(f"{len(missing_catalog)} components have no catalog number; match their exact MPNs in JLCPCB.")
            print("Review part matches, polarity and rotations in JLCPCB's assembly preview.")
            print("Saved CAD was exported without refilling zones or running ERC/DRC; review FABRICATION.md.")
        return 0
    except (OSError, ValueError, KeyError, csv.Error, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
