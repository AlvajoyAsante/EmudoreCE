#!/usr/bin/env python3
"""Prepare calculator-oriented AppVars without guessing which game code is unused."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import subprocess

import rom_to_appvar as d64
import disk_metadata
import disk_optimizations
import files_to_dsk_appvar as files_converter
import appvar_builder as rta


@dataclass(frozen=True)
class DiskFile:
    name: bytes
    data: bytes


@dataclass(frozen=True)
class BytePatch:
    offset: int
    expected: bytes
    replacement: bytes


@dataclass(frozen=True)
class ProfileRule:
    identifier: str
    description: str
    input_sha256: str
    output_sha256: str
    patches: tuple[BytePatch, ...]
    replacement: Path | None


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def checked_hash(value: object) -> str:
    if not isinstance(value, str) or len(value) != 64:
        raise ValueError("profile SHA-256 values must contain 64 hex digits")
    if any(character not in "0123456789abcdefABCDEF" for character in value):
        raise ValueError("profile SHA-256 values must contain 64 hex digits")
    return value.lower()


def load_profile(path: Path) -> list[ProfileRule]:
    profile = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(profile, dict) or set(profile) != {"version", "name", "rules"}:
        raise ValueError("profile requires version, name, and rules fields only")
    if type(profile["version"]) is not int or profile["version"] != 1:
        raise ValueError("unsupported optimization profile version")
    if not isinstance(profile["name"], str) or not profile["name"].strip():
        raise ValueError("profile name must be a nonempty string")
    if not isinstance(profile["rules"], list) or not profile["rules"]:
        raise ValueError("profile rules must be a nonempty array")

    rules = []
    identifiers = set()
    input_hashes = set()
    required = {"id", "description", "input_sha256", "output_sha256"}
    for entry in profile["rules"]:
        if not isinstance(entry, dict) or set(entry) not in (
            required | {"patches"}, required | {"replacement"}
        ):
            raise ValueError("each rule needs id, description, both hashes, and patches OR replacement")
        identifier = entry["id"]
        description = entry["description"]
        if not isinstance(identifier, str) or not identifier.strip():
            raise ValueError("rule id must be a nonempty string")
        if not isinstance(description, str) or not description.strip():
            raise ValueError(f"rule {identifier}: description must be nonempty")
        input_hash = checked_hash(entry["input_sha256"])
        output_hash = checked_hash(entry["output_sha256"])
        if identifier in identifiers or input_hash in input_hashes:
            raise ValueError("profile contains a duplicate rule id or input hash")
        if input_hash == output_hash:
            raise ValueError(f"rule {identifier}: input and output hashes must differ")
        identifiers.add(identifier)
        input_hashes.add(input_hash)

        patches = []
        replacement = None
        if "replacement" in entry:
            value = entry["replacement"]
            if not isinstance(value, str) or not value:
                raise ValueError(f"rule {identifier}: replacement must be a relative file path")
            relative_path = Path(value)
            replacement = (path.parent / relative_path).resolve()
            if relative_path.is_absolute() or not replacement.is_relative_to(path.parent.resolve()):
                raise ValueError(f"rule {identifier}: replacement must be inside the profile directory")
        else:
            values = entry["patches"]
            if not isinstance(values, list) or not values:
                raise ValueError(f"rule {identifier}: patches must be a nonempty array")
            for patch in values:
                if not isinstance(patch, dict) or set(patch) != {"offset", "expected", "replacement"}:
                    raise ValueError(f"rule {identifier}: invalid patch fields")
                offset = patch["offset"]
                if type(offset) is not int or offset < 0:
                    raise ValueError(f"rule {identifier}: offset must be a nonnegative integer")
                if not isinstance(patch["expected"], str) or not isinstance(patch["replacement"], str):
                    raise ValueError(f"rule {identifier}: patch bytes must be hex strings")
                expected = bytes.fromhex(patch["expected"])
                changed = bytes.fromhex(patch["replacement"])
                if not expected or len(expected) != len(changed):
                    raise ValueError(f"rule {identifier}: patch lengths must be equal and nonzero")
                patches.append(BytePatch(offset, expected, changed))
            patches.sort(key=lambda patch: patch.offset)
            for previous, current in zip(patches, patches[1:]):
                if previous.offset + len(previous.expected) > current.offset:
                    raise ValueError(f"rule {identifier}: patches overlap")

        rules.append(ProfileRule(identifier, description, input_hash, output_hash, tuple(patches), replacement))
    return rules


def apply_profile(data: bytes, rules: list[ProfileRule], *, prg: bool) -> tuple[bytes, ProfileRule | None]:
    digest = sha256(data)
    rule = next((candidate for candidate in rules if candidate.input_sha256 == digest), None)
    if rule is None:
        return data, None

    if rule.replacement is not None:
        changed = rule.replacement.read_bytes()
    else:
        patched = bytearray(data)
        for patch in rule.patches:
            end = patch.offset + len(patch.expected)
            if data[patch.offset:end] != patch.expected:
                raise ValueError(f"rule {rule.identifier}: expected bytes mismatch at offset {patch.offset}")
            patched[patch.offset:end] = patch.replacement
        changed = bytes(patched)

    if sha256(changed) != rule.output_sha256:
        raise ValueError(f"rule {rule.identifier}: output SHA-256 mismatch")
    if prg and (len(changed) < 2 or changed[:2] != data[:2]):
        raise ValueError(f"rule {rule.identifier}: PRG load address must remain unchanged")
    return changed, rule


def read_payload(payload: bytes) -> list[DiskFile]:
    if not payload or not 1 <= payload[0] <= d64.MAX_FILES:
        raise ValueError("invalid DSK file count")
    directory_end = 1 + payload[0] * d64.DIRECTORY_ENTRY
    if len(payload) < directory_end:
        raise ValueError("truncated DSK directory")

    files = []
    for index in range(payload[0]):
        entry = 1 + index * d64.DIRECTORY_ENTRY
        offset = int.from_bytes(payload[entry + 16:entry + 19], "little")
        size = int.from_bytes(payload[entry + 19:entry + 22], "little")
        if size < 2:
            raise ValueError(f"DSK file {index} has no load address")
        if offset < directory_end or offset + size > len(payload):
            raise ValueError(f"DSK file {index} is outside the payload")
        files.append(DiskFile(payload[entry:entry + 16], payload[offset:offset + size]))
    return files


def build_payload(files: list[DiskFile], *, deduplicate: bool = True) -> bytes:
    if not 1 <= len(files) <= d64.MAX_FILES:
        raise ValueError("invalid DSK file count")
    directory = bytearray([len(files)])
    directory_end = 1 + len(files) * d64.DIRECTORY_ENTRY
    blobs = bytearray()
    offsets: dict[bytes, int] = {}

    for entry in files:
        if len(entry.name) != 16 or len(entry.data) < 2:
            raise ValueError("DSK entries require a 16-byte name and a load address")
        offset = offsets.get(entry.data) if deduplicate else None
        if offset is None:
            offset = directory_end + len(blobs)
            if offset + len(entry.data) > 0xFFFFFF:
                raise ValueError("DSK payload exceeds the 24-bit size limit")
            offsets[entry.data] = offset
            blobs.extend(entry.data)
        directory.extend(entry.name)
        directory.extend(offset.to_bytes(3, "little"))
        directory.extend(len(entry.data).to_bytes(3, "little"))

    return bytes(directory + blobs)


def optimize_payload(payload: bytes) -> bytes:
    files = read_payload(payload)
    optimized = build_payload(files)
    if read_payload(optimized) != files:
        raise ValueError("optimized DSK round-trip verification failed")
    return optimized


def describe_file(name: str, original: bytes, changed: bytes, rule: ProfileRule | None) -> dict:
    return {
        "name": name,
        "original_sha256": sha256(original),
        "output_sha256": sha256(changed),
        "original_bytes": len(original),
        "output_bytes": len(changed),
        "changed": original != changed,
        "automatic_optimizations": [],
        "rule": None if rule is None else {
            "id": rule.identifier,
            "description": rule.description,
            "type": "replacement" if rule.replacement is not None else "patches",
        },
    }


def prepare_conversion(
    inputs: list[Path], kind: str = "dsk", profile: Path | None = None
) -> tuple[bytes, dict]:
    if not inputs:
        raise ValueError("no input files")
    if kind not in rta.KINDS:
        raise ValueError(f"unsupported package kind: {kind}")
    rules = load_profile(profile) if profile is not None else []
    records = []
    shared_bytes = 0

    if kind == "dsk":
        source_entries = None
        if len(inputs) == 1 and inputs[0].suffix.lower() == ".d64":
            image = inputs[0].read_bytes()
            if len(image) not in (d64.D64_SIZE_35_TRACKS, d64.D64_SIZE_35_TRACKS + 683):
                raise ValueError("expected a 35-track D64, optionally with 683 error bytes")
            original_payload = d64.build_payload(image)
            source_entries = d64.read_directory_entries(image)
        else:
            original_payload = files_converter.build_payload(inputs)
        original_files = read_payload(original_payload)
        changed_files = []
        for index, original in enumerate(original_files):
            changed, rule = apply_profile(original.data, rules, prg=True)
            automatic = None
            if rule is None:
                changed, automatic = disk_optimizations.optimize_mrcross(changed)
            changed_files.append(DiskFile(original.name, changed))
            record = describe_file(original.name.rstrip(b"\xa0").decode("latin-1"), original.data, changed, rule)
            if automatic is not None:
                record["automatic_optimizations"].append(automatic)
            record.update({
                "directory_index": index,
                "petscii_name_hex": original.name.hex(),
                "load_address": int.from_bytes(original.data[:2], "little"),
            })
            if source_entries is not None:
                record["source_file_type"] = source_entries[index].file_type
            records.append(record)
        unshared = build_payload(changed_files, deduplicate=False)
        payload = optimize_payload(unshared)
        shared_bytes = len(unshared) - len(payload)
    else:
        if len(inputs) != 1:
            raise ValueError(f"{kind} conversion requires exactly one input file")
        original_payload = inputs[0].read_bytes()
        rta.validate_rom(original_payload, kind)
        payload, rule = apply_profile(original_payload, rules, prg=False)
        rta.validate_rom(payload, kind)
        records.append(describe_file(inputs[0].name, original_payload, payload, rule))

    used_rules = {record["rule"]["id"] for record in records if record["rule"] is not None}
    changed_count = sum(record["changed"] for record in records)
    profile_count = sum(record["rule"] is not None for record in records)
    automatic_count = sum(bool(record["automatic_optimizations"]) for record in records)
    notes = [
        "No automatic code stripping, music removal, intro bypass, or arbitrary game depacking.",
    ]
    if kind == "dsk":
        notes.append("Normal DSK packages are already uncompressed; sharing identical files only saves archive space.")
    else:
        notes.append("Uncompressed packaging avoids package LZSS decoding at startup, at the cost of archive space.")
    if automatic_count:
        notes.append("Recognized decoder optimization reduces startup work, not the game's fight renderer.")
        notes.append("Reported guest-cycle savings are instruction counts, not measured calculator FPS.")
    if profile_count:
        notes.append("Profile hashes establish file identity; user-supplied changes still require behavioral validation.")
    if not changed_count:
        notes.append("No code or asset bytes changed; game execution and graphics decoding are unchanged.")

    report = {
        "report_version": 2,
        "mode": "calculator",
        "optimization_status": "code-transformed" if changed_count else "storage-only" if shared_bytes else "unchanged",
        "kind": kind,
        "inputs": [str(path) for path in inputs],
        "profile": str(profile) if profile is not None else None,
        "compression": "none",
        "original_payload_bytes": len(original_payload),
        "optimized_payload_bytes": len(payload),
        "identical_file_bytes_saved": shared_bytes,
        "files_changed": changed_count,
        "files_changed_automatically": automatic_count,
        "files_changed_by_profile": profile_count,
        "unmatched_rules": [rule.identifier for rule in rules if rule.identifier not in used_rules],
        "files": records,
        "notes": notes,
    }
    return payload, report


def convert(
    inputs: list[Path], output_dir: Path, kind: str, convbin: str, profile: Path | None = None,
    *, require_code_change: bool = False
) -> list[Path]:
    payload, report = prepare_conversion(inputs, kind, profile)
    if require_code_change and not report["files_changed"]:
        raise ValueError("no supported code optimization matched; no new AppVars written. Use normal conversion for this input.")
    metadata = None
    if kind == "dsk" and len(inputs) == 1 and inputs[0].suffix.lower() == ".d64":
        ordered_records = sorted(report["files"], key=lambda record: record["directory_index"])
        file_types = bytes(record["source_file_type"] for record in ordered_records)
        metadata = disk_metadata.build_metadata(payload, file_types)
    chunks = rta.build_chunks(payload, kind, len(payload), rta.COMPRESSION_NONE)
    package_id = rta.KINDS[kind][0]
    report_path = output_dir / f"optimization-{kind}.json"
    destinations = [output_dir / f"EC{package_id}{index:02X}.8xv" for index in range(len(chunks))]
    if metadata is not None:
        destinations.append(output_dir / f"{disk_metadata.VARIABLE_NAME}.8xv")
    destinations.append(report_path)
    sources = list(inputs)
    if profile is not None:
        sources.append(profile)
        sources.extend(rule.replacement for rule in load_profile(profile) if rule.replacement is not None)
    rta.validate_output_paths(destinations, sources)
    outputs = rta.write_chunks(chunks, package_id, output_dir, convbin)
    if metadata is not None:
        outputs.append(rta.write_appvar(metadata, disk_metadata.VARIABLE_NAME, output_dir, convbin))
    report["appvars"] = [path.name for path in outputs]
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{kind}: {len(payload)} uncompressed bytes")
    if report["files_changed"]:
        print(f"Code transformations: {report['files_changed']} file(s); "
              f"automatic: {report['files_changed_automatically']}, explicit profile: {report['files_changed_by_profile']}")
        for record in report["files"]:
            for optimization in record["automatic_optimizations"]:
                print(f"  {record['name']!r}: {optimization['id']}")
                print(f"  {optimization['bank_writes_removed']:,} redundant bank writes removed; "
                      f"{optimization['guest_cycles_saved']:,} guest cycles saved during startup decompression.")
    else:
        print("NO CODE OPTIMIZATION APPLIED: no supported packer or explicit profile matched.")
    print(f"Archive bytes saved by file sharing: {report['identical_file_bytes_saved']} (separate from execution savings)")
    for note in report["notes"]:
        print(note)
    return outputs + [report_path]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="one .d64, .prg/.bas files, or one ROM with --kind")
    parser.add_argument("--kind", choices=tuple(rta.KINDS), default="dsk")
    parser.add_argument("--output", type=Path, default=Path("bin/optimized"))
    parser.add_argument("--convbin", help="path to the CEdev convbin executable")
    parser.add_argument("--profile", type=Path, help="explicit hash-checked JSON patch/replacement profile")
    parser.add_argument("--require-code-change", action="store_true", help="fail without writing a package when no code transformation applies")
    parser.add_argument("--dry-run", action="store_true", help="print the JSON report; do not write files or run convbin")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.dry_run:
            _, report = prepare_conversion(args.inputs, args.kind, args.profile)
            if args.require_code_change and not report["files_changed"]:
                raise ValueError("no supported code optimization matched; no files written")
            print(json.dumps(report, indent=2))
        else:
            outputs = convert(args.inputs, args.output, args.kind, rta.find_convbin(args.convbin), args.profile,
                              require_code_change=args.require_code_change)
            for output in outputs:
                print(output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"error: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())