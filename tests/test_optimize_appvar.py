from contextlib import redirect_stdout
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import optimize_appvar as optimizer
import appvar_builder as rta
from test_rom_to_appvar import make_d64


def disk_file(name, data):
    return optimizer.DiskFile(name.ljust(16, b"\xa0"), data)


class OptimizedPayloadTests(unittest.TestCase):
    def test_aliases_keep_names_order_load_addresses_and_bytes(self):
        data = b"\x01\x08" + bytes(range(256))
        files = [
            disk_file(b"BOOT", data),
            disk_file(b"PART", b"\x00\x40different"),
            disk_file(b"ALIAS", data),
            disk_file(b"BOOT", data),
        ]
        normal = optimizer.build_payload(files, deduplicate=False)
        optimized = optimizer.optimize_payload(normal)
        self.assertEqual(optimizer.read_payload(optimized), files)
        self.assertEqual(len(normal) - len(optimized), len(data) * 2)
        offsets = [int.from_bytes(optimized[17 + index * 22:20 + index * 22], "little")
                   for index in range(4)]
        self.assertEqual(offsets[0], offsets[2])
        self.assertEqual(offsets[0], offsets[3])
        self.assertNotEqual(offsets[0], offsets[1])

    def test_different_load_addresses_are_not_merged(self):
        files = [disk_file(b"ONE", b"\x00\x20same"), disk_file(b"TWO", b"\x00\x40same")]
        normal = optimizer.build_payload(files, deduplicate=False)
        self.assertEqual(optimizer.optimize_payload(normal), normal)

    def test_optimization_is_idempotent(self):
        files = [disk_file(b"ONE", b"\x00\x20same"), disk_file(b"TWO", b"\x00\x20same")]
        optimized = optimizer.build_payload(files)
        self.assertEqual(optimizer.optimize_payload(optimized), optimized)

    def test_shared_data_crosses_appvar_chunk_boundaries(self):
        data = b"\x00\x20" + bytes(range(256)) * 200
        files = [disk_file(b"ONE", data), disk_file(b"TWO", data)]
        payload = optimizer.build_payload(files)
        chunks = rta.build_chunks(payload, "dsk", len(payload), rta.COMPRESSION_NONE)
        self.assertEqual(len(chunks), 2)
        rebuilt = b"".join(chunk[rta.HEADER.size:] for chunk in chunks)
        self.assertEqual(optimizer.read_payload(rebuilt), files)

    def test_all_144_directory_entries_remain(self):
        files = [disk_file(str(index).encode("ascii"), b"\x00\x20data") for index in range(144)]
        self.assertEqual(optimizer.read_payload(optimizer.build_payload(files)), files)

    def test_rejects_invalid_counts_and_truncated_directory(self):
        for payload in (b"", b"\x00", b"\xff", b"\x01short"):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                optimizer.read_payload(payload)
        for files in ([], [disk_file(b"TOO MANY", b"ab")] * 145):
            with self.assertRaises(ValueError):
                optimizer.build_payload(files)

    def test_rejects_out_of_bounds_file_data(self):
        original = optimizer.build_payload([disk_file(b"ONE", b"\x01\x08data")])
        for offset in (0, 1, 22, len(original), 0xFFFFFF):
            payload = bytearray(original)
            payload[17:20] = offset.to_bytes(3, "little")
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "outside"):
                optimizer.read_payload(bytes(payload))

    def test_rejects_missing_load_address(self):
        payload = bytearray(optimizer.build_payload([disk_file(b"ONE", b"ab")]))
        payload[20:23] = (1).to_bytes(3, "little")
        with self.assertRaisesRegex(ValueError, "load address"):
            optimizer.read_payload(bytes(payload))
        with self.assertRaises(ValueError):
            optimizer.build_payload([disk_file(b"ONE", b"a")])


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.profile_path = self.directory / "profile.json"
        self.original = b"\x01\x08original-data"
        self.changed = b"\x01\x08replaced-data"
        self.rule = {
            "id": "synthetic-test",
            "description": "Synthetic data replacement, not a game optimization",
            "input_sha256": optimizer.sha256(self.original),
            "output_sha256": optimizer.sha256(self.changed),
            "patches": [{"offset": 2, "expected": b"original".hex(), "replacement": b"replaced".hex()}],
        }

    def load_rules(self, rules=None):
        self.profile_path.write_text(json.dumps({
            "version": 1, "name": "Synthetic tests", "rules": rules if rules is not None else [self.rule]
        }), encoding="utf-8")
        return optimizer.load_profile(self.profile_path)

    def test_verified_patch(self):
        changed, rule = optimizer.apply_profile(self.original, self.load_rules(), prg=True)
        self.assertEqual(changed, self.changed)
        self.assertEqual(rule.identifier, "synthetic-test")

    def test_unknown_build_is_unchanged(self):
        unknown = self.original + b"another version"
        self.assertEqual(optimizer.apply_profile(unknown, self.load_rules(), prg=True), (unknown, None))

    def test_rejects_expected_byte_mismatch(self):
        self.rule["patches"][0]["expected"] = bytes(8).hex()
        with self.assertRaisesRegex(ValueError, "expected bytes mismatch"):
            optimizer.apply_profile(self.original, self.load_rules(), prg=True)

    def test_rejects_wrong_output_hash(self):
        self.rule["output_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "output SHA-256 mismatch"):
            optimizer.apply_profile(self.original, self.load_rules(), prg=True)

    def test_rejects_truncated_expected_range(self):
        self.rule["patches"][0]["offset"] = len(self.original) - 1
        with self.assertRaisesRegex(ValueError, "expected bytes mismatch"):
            optimizer.apply_profile(self.original, self.load_rules(), prg=True)

    def test_rejects_overlap_length_change_and_bad_offset(self):
        invalid_patches = [
            [{"offset": 2, "expected": "0000", "replacement": "0101"},
             {"offset": 3, "expected": "00", "replacement": "01"}],
            [{"offset": 2, "expected": "0000", "replacement": "01"}],
            [{"offset": -1, "expected": "00", "replacement": "01"}],
            [{"offset": True, "expected": "00", "replacement": "01"}],
            [{"offset": 2, "expected": "", "replacement": ""}],
            [{"offset": 2, "expected": "not hex", "replacement": "00"}],
        ]
        for patches in invalid_patches:
            self.rule["patches"] = patches
            with self.subTest(patches=patches), self.assertRaises(ValueError):
                self.load_rules()

    def test_rejects_duplicate_rules_and_bad_hashes(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.load_rules([self.rule, self.rule])
        for digest in ("bad", "z" * 64, 123, " " * 64):
            self.rule["input_sha256"] = digest
            with self.subTest(digest=digest), self.assertRaisesRegex(ValueError, "SHA-256"):
                self.load_rules()

    def test_rejects_unknown_or_ambiguous_fields(self):
        self.rule["replacement"] = "replacement.prg"
        with self.assertRaisesRegex(ValueError, "patches OR replacement"):
            self.load_rules()
        del self.rule["replacement"]
        self.rule["surprise"] = True
        with self.assertRaises(ValueError):
            self.load_rules()

    def test_replacement_is_hash_checked(self):
        del self.rule["patches"]
        self.rule["replacement"] = "unpacked.prg"
        replacement = self.directory / "unpacked.prg"
        replacement.write_bytes(self.changed)
        changed, rule = optimizer.apply_profile(self.original, self.load_rules(), prg=True)
        self.assertEqual(changed, self.changed)
        self.assertIsNotNone(rule)
        replacement.write_bytes(b"wrong replacement")
        with self.assertRaisesRegex(ValueError, "output SHA-256 mismatch"):
            optimizer.apply_profile(self.original, self.load_rules(), prg=True)

    def test_rejects_replacement_outside_profile(self):
        del self.rule["patches"]
        self.rule["replacement"] = "../elsewhere.prg"
        with self.assertRaisesRegex(ValueError, "inside the profile directory"):
            self.load_rules()

    def test_unknown_build_does_not_read_replacement(self):
        del self.rule["patches"]
        self.rule["replacement"] = "does-not-exist.prg"
        self.assertEqual(optimizer.apply_profile(b"unknown", self.load_rules(), prg=True), (b"unknown", None))

    def test_rejects_prg_load_address_change(self):
        self.changed = b"\x00\x40" + self.original[2:]
        self.rule["output_sha256"] = optimizer.sha256(self.changed)
        self.rule["patches"] = [{"offset": 0, "expected": "0108", "replacement": "0040"}]
        with self.assertRaisesRegex(ValueError, "load address"):
            optimizer.apply_profile(self.original, self.load_rules(), prg=True)


class ConversionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.original = b"\x01\x08" + bytes(range(256))
        self.program = self.directory / "game.prg"
        self.program.write_bytes(self.original)

    def test_normal_and_optimized_prg_contents_match(self):
        payload, report = optimizer.prepare_conversion([self.program])
        normal = optimizer.files_converter.build_payload([self.program])
        self.assertEqual(payload, normal)
        self.assertEqual(report["files_changed_by_profile"], 0)
        self.assertEqual(report["files"][0]["original_sha256"], optimizer.sha256(self.original))
        self.assertEqual(self.program.read_bytes(), self.original)

    def test_reports_storage_savings_without_code_changes(self):
        duplicate = self.directory / "other.prg"
        duplicate.write_bytes(self.original)
        payload, report = optimizer.prepare_conversion([self.program, duplicate])
        self.assertEqual(report["identical_file_bytes_saved"], len(self.original))
        self.assertEqual(report["files_changed_by_profile"], 0)
        self.assertEqual([entry.data for entry in optimizer.read_payload(payload)], [self.original] * 2)

    def test_d64_uses_normal_extraction_with_names_and_order_preserved(self):
        image = make_d64([(b"BOOT", 0x82, self.original), (b"ALIAS", 0x82, self.original)])
        disk = self.directory / "arbitrary.d64"
        for suffix in (b"", bytes(683)):
            disk.write_bytes(image + suffix)
            payload, report = optimizer.prepare_conversion([disk])
            normal = optimizer.d64.build_payload(image)
            self.assertEqual(optimizer.read_payload(payload), optimizer.read_payload(normal))
            self.assertEqual(report["identical_file_bytes_saved"], len(self.original))
            self.assertEqual(disk.read_bytes(), image + suffix)

    def test_basic_uses_existing_tokenizer(self):
        listing = self.directory / "example.bas"
        listing.write_text('10 PRINT "EXAMPLE"\n20 GOTO 10\n', encoding="utf-8")
        payload, _ = optimizer.prepare_conversion([listing, self.program])
        self.assertEqual(payload, optimizer.files_converter.build_payload([listing, self.program]))

    def test_roms_preserve_exact_bytes_and_disable_package_compression(self):
        for kind, size in (("basic", 8192), ("kernal", 8192), ("chars", 4096), ("cart", 5000)):
            original = bytes(size)
            self.program.write_bytes(original)
            with self.subTest(kind=kind):
                payload, report = optimizer.prepare_conversion([self.program], kind)
                self.assertEqual(payload, original)
                self.assertEqual(report["compression"], "none")
                self.assertEqual(report["files_changed_by_profile"], 0)

    def test_rejects_invalid_inputs(self):
        with self.assertRaisesRegex(ValueError, "no input"):
            optimizer.prepare_conversion([])
        with self.assertRaisesRegex(ValueError, "unsupported package"):
            optimizer.prepare_conversion([self.program], "unknown")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            optimizer.prepare_conversion([self.program, self.program], "basic")
        with self.assertRaisesRegex(ValueError, "exactly"):
            optimizer.prepare_conversion([self.program], "basic")
        disk = self.directory / "bad.d64"
        disk.write_bytes(b"invalid disk")
        with self.assertRaisesRegex(ValueError, "35-track"):
            optimizer.prepare_conversion([disk])

    def test_profile_changes_only_matching_file_and_reports_unused_rules(self):
        changed = self.original[:2] + b"\xff" + self.original[3:]
        profile = self.directory / "profile.json"
        rule = {
            "id": "synthetic", "description": "Synthetic test only",
            "input_sha256": optimizer.sha256(self.original),
            "output_sha256": optimizer.sha256(changed),
            "patches": [{"offset": 2, "expected": "00", "replacement": "ff"}],
        }
        profile.write_text(json.dumps({"version": 1, "name": "Test", "rules": [rule]}), encoding="utf-8")
        other = self.directory / "other.prg"
        other.write_bytes(b"\x01\x08unknown build")
        payload, report = optimizer.prepare_conversion([self.program, other], profile=profile)
        self.assertEqual([entry.data for entry in optimizer.read_payload(payload)], [changed, other.read_bytes()])
        self.assertEqual(report["files_changed_by_profile"], 1)
        self.assertEqual(report["unmatched_rules"], [])
        _, unknown_report = optimizer.prepare_conversion([other], profile=profile)
        self.assertEqual(unknown_report["files_changed_by_profile"], 0)
        self.assertEqual(unknown_report["unmatched_rules"], ["synthetic"])
        self.assertEqual(self.program.read_bytes(), self.original)

    def test_convert_writes_uncompressed_chunks_and_report(self):
        output = self.directory / "optimized"

        def fake_write(chunks, package_id, output_dir, convbin):
            self.assertEqual(package_id, "DSK")
            self.assertEqual(convbin, "test-convbin")
            self.assertEqual(rta.HEADER.unpack_from(chunks[0])[6], rta.COMPRESSION_NONE)
            output_dir.mkdir()
            return [output_dir / "ECDSK00.8xv"]

        with mock.patch.object(rta, "write_chunks", side_effect=fake_write), redirect_stdout(io.StringIO()):
            outputs = optimizer.convert([self.program], output, "dsk", "test-convbin")
        self.assertEqual(outputs, [output / "ECDSK00.8xv", output / "optimization-dsk.json"])
        report = json.loads(outputs[-1].read_text(encoding="utf-8"))
        self.assertEqual(report["appvars"], ["ECDSK00.8xv"])
        self.assertEqual(report["files"][0]["output_sha256"], optimizer.sha256(self.original))

    def test_dry_run_never_writes_or_resolves_convbin(self):
        output = self.directory / "must-not-exist"
        arguments = ["optimize_appvar.py", str(self.program), "--dry-run", "--output", str(output)]
        captured = io.StringIO()
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(rta, "find_convbin") as find:
            with mock.patch.object(rta, "write_chunks") as write, redirect_stdout(captured):
                self.assertEqual(optimizer.main(), 0)
        find.assert_not_called()
        write.assert_not_called()
        self.assertFalse(output.exists())
        self.assertEqual(json.loads(captured.getvalue())["files_changed_by_profile"], 0)

    def test_refuses_to_overwrite_source_rom(self):
        source = self.directory / "ECCHR00.8xv"
        original = bytes(4096)
        source.write_bytes(original)
        with mock.patch.object(rta, "write_chunks") as write:
            with self.assertRaisesRegex(ValueError, "overwrite a source"):
                optimizer.convert([source], self.directory, "chars", "unused")
        write.assert_not_called()
        self.assertEqual(source.read_bytes(), original)

    def test_refuses_to_overwrite_profile_or_replacement(self):
        profile = self.directory / "optimization-dsk.json"
        replacement = self.directory / "ECDSK00.8xv"
        changed = self.original + b"synthetic replacement"
        replacement.write_bytes(changed)
        profile.write_text(json.dumps({
            "version": 1, "name": "Test", "rules": [{
                "id": "synthetic", "description": "Synthetic test only",
                "input_sha256": optimizer.sha256(self.original),
                "output_sha256": optimizer.sha256(changed),
                "replacement": replacement.name,
            }]
        }), encoding="utf-8")
        original_profile = profile.read_bytes()
        with mock.patch.object(rta, "write_chunks") as write:
            with self.assertRaisesRegex(ValueError, "overwrite a source"):
                optimizer.convert([self.program], self.directory, "dsk", "unused", profile)
            renamed = self.directory / "profile.json"
            profile.rename(renamed)
            with self.assertRaisesRegex(ValueError, "overwrite a source"):
                optimizer.convert([self.program], self.directory, "dsk", "unused", renamed)
        write.assert_not_called()
        self.assertEqual(replacement.read_bytes(), changed)
        self.assertEqual(renamed.read_bytes(), original_profile)

    def test_bad_matching_profile_aborts_before_appvar_writes(self):
        profile = self.directory / "profile.json"
        profile.write_text(json.dumps({
            "version": 1, "name": "Test", "rules": [{
                "id": "synthetic", "description": "Synthetic test only",
                "input_sha256": optimizer.sha256(self.original),
                "output_sha256": "0" * 64,
                "patches": [{"offset": 2, "expected": "00", "replacement": "ff"}],
            }]
        }), encoding="utf-8")
        output = self.directory / "must-not-exist"
        with mock.patch.object(rta, "write_chunks") as write:
            with self.assertRaisesRegex(ValueError, "output SHA-256 mismatch"):
                optimizer.convert([self.program], output, "dsk", "unused", profile)
        write.assert_not_called()
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()