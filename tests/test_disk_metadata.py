from contextlib import redirect_stdout
import importlib
import io
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

d64 = importlib.import_module("d64_to_appvar")
metadata = importlib.import_module("disk_metadata")
optimizer = importlib.import_module("optimize_appvar")
rta = importlib.import_module("appvar_builder")
from test_rom_to_appvar import make_d64


def payload_for(data=b"\x01\x08data", count=1):
    directory_end = 1 + count * 22
    entry = b"CODE.OBJ".ljust(16, b"\xa0")
    entry += directory_end.to_bytes(3, "little") + len(data).to_bytes(3, "little")
    return bytes([count]) + entry * count + data


class DirectoryMetadataTests(unittest.TestCase):
    def setUp(self):
        self.program = b"\x01\x08\x60"
        self.image = make_d64([
            (b"CODE.OBJ", 0x82, self.program),
            (b"NOTES", 0x81, b"seq"),
            (b"CODE.OBJ", 0xC2, self.program),
            (b"OPEN", 0x02, self.program),
        ])

    def test_original_types_and_obj_prg_retained_in_order(self):
        entries = d64.read_directory_entries(self.image)
        self.assertEqual([entry.file_type for entry in entries], [0x82, 0xC2])
        self.assertEqual([entry.name for entry in entries], [b"CODE.OBJ".ljust(16, b"\xa0")] * 2)
        self.assertEqual([bool(entry.file_type & metadata.FILE_TYPE_LOCKED) for entry in entries],
                         [False, True])

    def test_legacy_tuple_api_unchanged(self):
        name = b"CODE.OBJ".ljust(16, b"\xa0")
        self.assertEqual(d64.read_directory(self.image), [(name, 1, 0), (name, 1, 2)])

    def test_normal_payload_exact_bytes_independent_of_lock(self):
        name = b"CODE.OBJ".ljust(16, b"\xa0")
        expected = b"\x02" + name + b"\x2d\x00\x00\x03\x00\x00"
        expected += name + b"\x30\x00\x00\x03\x00\x00" + self.program * 2
        self.assertEqual(d64.build_payload(self.image), expected)
        unlocked = bytearray(self.image)
        unlocked[d64.sector_offset(18, 1) + 2 + 2 * 32] = 0x82
        self.assertEqual(d64.build_payload(bytes(unlocked)), expected)

    def test_duplicate_names_and_shared_data_keep_flags_by_index(self):
        payload = optimizer.optimize_payload(d64.build_payload(self.image))
        entries = d64.read_directory_entries(self.image)
        file_types = bytes(entry.file_type for entry in entries)
        self.assertEqual(payload[17:20], payload[39:42])
        encoded = metadata.build_metadata(payload, file_types)
        self.assertEqual(metadata.validate_metadata(encoded, payload), b"\x82\xc2")


class AppVarWriterTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.output = Path(temporary.name) / "output"
        self.calls = []

    def capture_convbin(self, command, *, check):
        self.assertTrue(check)
        raw = Path(command[command.index("-i") + 1])
        self.calls.append((command, raw.read_bytes()))

    def assert_invocation(self, call, name, data):
        command, written = call
        raw = Path(command[6])
        self.assertEqual(raw.name, name + ".bin")
        self.assertEqual(command, ["test-convbin", "-j", "bin", "-k", "8xv", "-i", str(raw),
                                   "-o", str(self.output / (name + ".8xv")), "-n", name, "-r"])
        self.assertEqual(written, data)

    def test_standalone_appvar_has_no_chunk_suffix_or_package_header(self):
        data = metadata.build_metadata(payload_for(), b"\xc2")
        with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
            output = rta.write_appvar(data, "ECDINFO", self.output, "test-convbin")
        self.assertEqual(output, self.output / "ECDINFO.8xv")
        self.assertEqual(len(self.calls), 1)
        self.assert_invocation(self.calls[0], "ECDINFO", data)

    def test_chunk_names_options_and_bytes_unchanged(self):
        data = bytes(range(256)) * 140
        chunks = rta.build_chunks(data, "dsk", len(data), rta.COMPRESSION_NONE)
        with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
            outputs = rta.write_chunks(chunks, "DSK", self.output, "test-convbin")
        self.assertEqual(outputs, [self.output / "ECDSK00.8xv", self.output / "ECDSK01.8xv"])
        for index, chunk in enumerate(chunks):
            self.assert_invocation(self.calls[index], f"ECDSK{index:02X}", chunk)

    def test_empty_chunk_list_still_creates_output_directory(self):
        with mock.patch.object(rta.subprocess, "run") as run:
            self.assertEqual(rta.write_chunks([], "DSK", self.output, "unused"), [])
        run.assert_not_called()
        self.assertTrue(self.output.is_dir())


class ConversionMetadataTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.output = self.directory / "output"
        self.program = b"\x01\x08original"
        self.other = b"\x00\x40other"
        self.image = make_d64([
            (b"CODE.OBJ", 0x82, self.program),
            (b"NOTES", 0x81, b"seq"),
            (b"CODE.OBJ", 0xC2, self.program),
            (b"OPEN", 0x02, self.program),
            (b"OTHER", 0x82, self.other),
        ])
        self.disk = self.directory / "source.d64"
        self.disk.write_bytes(self.image)
        self.types = b"\x82\xc2\x82"
        self.appvars = {}

    def capture_convbin(self, command, *, check):
        self.assertTrue(check)
        data = Path(command[command.index("-i") + 1]).read_bytes()
        destination = Path(command[command.index("-o") + 1])
        self.appvars[destination.name] = data
        destination.write_bytes(data)

    def assert_disk_bytes(self, payload):
        chunks = rta.build_chunks(payload, "dsk", len(payload), rta.COMPRESSION_NONE)
        disk_names = [f"ECDSK{index:02X}.8xv" for index in range(len(chunks))]
        self.assertEqual([self.appvars[name] for name in disk_names], chunks)
        self.assertEqual(metadata.validate_metadata(self.appvars["ECDINFO.8xv"], payload), self.types)
        return disk_names

    def assert_refused_without_writes(self, action, message):
        with mock.patch.object(rta, "write_chunks") as chunks, mock.patch.object(rta, "write_appvar") as appvar:
            with redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, message):
                action()
        chunks.assert_not_called()
        appvar.assert_not_called()

    def make_profile(self, path=None, replacement=None):
        path = path or self.directory / "profile.json"
        changed = self.program[:-1] + b"X"
        rule = {
            "id": "synthetic", "description": "Synthetic test data only",
            "input_sha256": optimizer.sha256(self.program),
            "output_sha256": optimizer.sha256(changed),
        }
        if replacement is None:
            rule["patches"] = [{"offset": len(self.program) - 1,
                                "expected": self.program[-1:].hex(), "replacement": "58"}]
        else:
            replacement.write_bytes(changed)
            rule["replacement"] = str(replacement.relative_to(path.parent))
        path.write_text(json.dumps({"version": 1, "name": "Test", "rules": [rule]}), encoding="utf-8")
        return path

    def hardlink_or_skip(self, destination, source):
        try:
            destination.hardlink_to(source)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"hard links unavailable: {error}")

    def test_normal_conversion_lists_sidecar_and_keeps_exact_disk_bytes(self):
        payload = d64.build_payload(self.image)
        with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
            with redirect_stdout(io.StringIO()):
                outputs = d64.convert(self.disk, self.output, "test-convbin")
        disk_names = self.assert_disk_bytes(payload)
        self.assertEqual(outputs, [self.output / name for name in disk_names + ["ECDINFO.8xv"]])
        self.assertEqual(set(self.appvars), set(disk_names + ["ECDINFO.8xv"]))
        self.assertEqual(self.disk.read_bytes(), self.image)

    def test_optimized_report_and_shared_data_preserve_source_flag_indices(self):
        prepared = optimizer.prepare_conversion([self.disk])
        self.assertEqual(len(prepared), 2)
        payload, report = prepared
        self.assertEqual([record["directory_index"] for record in report["files"]], [0, 1, 2])
        self.assertEqual([record["source_file_type"] for record in report["files"]], list(self.types))
        self.assertEqual(payload[17:20], payload[39:42])
        with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
            with redirect_stdout(io.StringIO()):
                outputs = optimizer.convert([self.disk], self.output, "dsk", "test-convbin")
        disk_names = self.assert_disk_bytes(payload)
        names = disk_names + ["ECDINFO.8xv", "optimization-dsk.json"]
        self.assertEqual(outputs, [self.output / name for name in names])
        written_report = json.loads(outputs[-1].read_text(encoding="utf-8"))
        self.assertEqual(written_report["appvars"], names[:-1])
        self.assertEqual(written_report["files"], report["files"])

    def test_profile_changed_final_payload_and_dedup_determine_sidecar_identity(self):
        changed = self.program[:2] + bytes(range(256)) * 140
        replacement = self.directory / "replacement.prg"
        replacement.write_bytes(changed)
        profile = self.directory / "profile.json"
        profile.write_text(json.dumps({
            "version": 1, "name": "Synthetic replacement", "rules": [{
                "id": "synthetic", "description": "Synthetic test data only",
                "input_sha256": optimizer.sha256(self.program),
                "output_sha256": optimizer.sha256(changed),
                "replacement": replacement.name,
            }],
        }), encoding="utf-8")
        expected = optimizer.build_payload([
            optimizer.DiskFile(b"CODE.OBJ".ljust(16, b"\xa0"), changed),
            optimizer.DiskFile(b"CODE.OBJ".ljust(16, b"\xa0"), changed),
            optimizer.DiskFile(b"OTHER".ljust(16, b"\xa0"), self.other),
        ])
        original = d64.build_payload(self.image)
        self.assertNotEqual(len(expected), len(original))
        self.assertNotEqual(zlib.crc32(expected), zlib.crc32(original))
        with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
            with redirect_stdout(io.StringIO()):
                outputs = optimizer.convert([self.disk], self.output, "dsk", "test-convbin", profile,
                                            require_code_change=True)
        self.assertEqual(len(self.assert_disk_bytes(expected)), 2)
        with self.assertRaises(ValueError):
            metadata.validate_metadata(self.appvars["ECDINFO.8xv"], original)
        report = json.loads(outputs[-1].read_text(encoding="utf-8"))
        self.assertEqual(report["optimized_payload_bytes"], len(expected))
        self.assertEqual(report["identical_file_bytes_saved"], len(changed))
        self.assertEqual(report["files_changed_by_profile"], 2)
        self.assertEqual([record["source_file_type"] for record in report["files"]], list(self.types))
        self.assertEqual(self.disk.read_bytes(), self.image)

    def test_standalone_prg_bas_and_firmware_have_no_metadata_or_source_type(self):
        cases = [("prg", "dsk", self.program), ("bas", "dsk", b'10 PRINT "TEST"\n'),
                 ("bin", "basic", bytes(8192)), ("bin", "kernal", bytes(8192)),
                 ("bin", "chars", bytes(4096)), ("bin", "cart", bytes(100))]
        for index, (suffix, kind, data) in enumerate(cases):
            with self.subTest(suffix=suffix, kind=kind):
                source = self.directory / f"source{index}.{suffix}"
                source.write_bytes(data)
                output = self.directory / f"standalone{index}"
                self.appvars.clear()
                with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
                    with redirect_stdout(io.StringIO()):
                        outputs = optimizer.convert([source], output, kind, "test-convbin")
                report = json.loads(outputs[-1].read_text(encoding="utf-8"))
                self.assertNotIn("ECDINFO.8xv", self.appvars)
                self.assertEqual(report["appvars"], [path.name for path in outputs[:-1]])
                self.assertTrue(all("source_file_type" not in record for record in report["files"]))
                self.assertEqual(source.read_bytes(), data)

    def test_flags_only_change_sidecar_not_normal_or_optimized_chunks(self):
        unlocked = bytearray(self.image)
        unlocked[d64.sector_offset(18, 1) + 2 + 2 * 32] = 0x82
        for optimized in (False, True):
            previous = None
            for variant, image in enumerate((self.image, bytes(unlocked))):
                self.disk.write_bytes(image)
                output = self.directory / f"flags-{optimized}-{variant}"
                self.appvars.clear()
                with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
                    with redirect_stdout(io.StringIO()):
                        if optimized:
                            optimizer.convert([self.disk], output, "dsk", "test-convbin")
                        else:
                            d64.convert(self.disk, output, "test-convbin")
                if previous is not None:
                    with self.subTest(optimized=optimized):
                        self.assertEqual({name: data for name, data in self.appvars.items() if name.startswith("ECDSK")},
                                         {name: data for name, data in previous.items() if name.startswith("ECDSK")})
                        self.assertEqual(self.appvars["ECDINFO.8xv"][:16], previous["ECDINFO.8xv"][:16])
                        self.assertNotEqual(self.appvars["ECDINFO.8xv"], previous["ECDINFO.8xv"])
                previous = dict(self.appvars)

    def test_report_directory_index_controls_metadata_order(self):
        payload, report = optimizer.prepare_conversion([self.disk])
        report["files"].reverse()
        with mock.patch.object(optimizer, "prepare_conversion", return_value=(payload, report)):
            with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
                with redirect_stdout(io.StringIO()):
                    optimizer.convert([self.disk], self.output, "dsk", "test-convbin")
        self.assert_disk_bytes(payload)

    def test_d64_converter_keeps_144_file_limit(self):
        entries = [d64.DirectoryEntry(b"CODE.OBJ".ljust(16, b"\xa0"), 1, 0, 0xC2)] * 144
        with mock.patch.object(d64, "read_directory_entries", return_value=entries):
            with mock.patch.object(rta.subprocess, "run", side_effect=self.capture_convbin):
                with redirect_stdout(io.StringIO()):
                    d64.convert(self.disk, self.output, "test-convbin")
            payload = d64.build_payload(self.image)
            self.assertEqual(metadata.validate_metadata(self.appvars["ECDINFO.8xv"], payload), b"\xc2" * 144)
            entries.append(entries[0])
            overflow = self.directory / "overflow"
            self.assert_refused_without_writes(lambda: d64.convert(self.disk, overflow, "unused"), "too many files")
            self.assertFalse(overflow.exists())

    def test_d64_dry_run_and_strict_dry_run_never_resolve_convbin_or_write(self):
        for strict in (False, True):
            arguments = ["optimize_appvar.py", str(self.disk), "--dry-run", "--output", str(self.output)]
            if strict:
                arguments.append("--require-code-change")
            captured = io.StringIO()
            with self.subTest(strict=strict), mock.patch.object(sys, "argv", arguments):
                with mock.patch.object(rta, "find_convbin") as find, mock.patch.object(rta, "write_chunks") as chunks:
                    with mock.patch.object(rta, "write_appvar") as appvar, redirect_stdout(captured):
                        if strict:
                            with self.assertRaisesRegex(SystemExit, "no supported code optimization"):
                                optimizer.main()
                        else:
                            self.assertEqual(optimizer.main(), 0)
                            report = json.loads(captured.getvalue())
                            self.assertEqual([record["source_file_type"] for record in report["files"]], list(self.types))
                find.assert_not_called()
                chunks.assert_not_called()
                appvar.assert_not_called()
                self.assertFalse(self.output.exists())

    def test_strict_refusal_creates_no_outputs(self):
        self.assert_refused_without_writes(
            lambda: optimizer.convert([self.disk], self.output, "dsk", "unused", require_code_change=True),
            "no supported code optimization")
        self.assertFalse(self.output.exists())

    def test_invalid_and_bad_matching_profiles_create_no_outputs(self):
        profile = self.make_profile()
        document = json.loads(profile.read_text(encoding="utf-8"))
        document["rules"][0]["output_sha256"] = "0" * 64
        for text in ("{", json.dumps(document)):
            with self.subTest(profile=text):
                profile.write_text(text, encoding="utf-8")
                self.assert_refused_without_writes(
                    lambda: optimizer.convert([self.disk], self.output, "dsk", "unused", profile), ".")
                self.assertFalse(self.output.exists())

    def test_refusal_keeps_preexisting_artifacts(self):
        self.output.mkdir()
        artifacts = {"ECDSK00.8xv": b"old disk", "ECDINFO.8xv": b"old metadata",
                     "optimization-dsk.json": b"old report"}
        for name, data in artifacts.items():
            (self.output / name).write_bytes(data)
        self.assert_refused_without_writes(
            lambda: optimizer.convert([self.disk], self.output, "dsk", "unused", require_code_change=True),
            "no supported code optimization")
        self.assertEqual({path.name: path.read_bytes() for path in self.output.iterdir()}, artifacts)

    def test_normal_metadata_destination_cannot_overwrite_input(self):
        source = self.directory / "ECDINFO.8xv"
        source.write_bytes(self.image)
        self.assert_refused_without_writes(lambda: d64.convert(source, self.directory, "unused"), "overwrite a source")
        self.assertEqual(source.read_bytes(), self.image)
        self.assertFalse((self.directory / "ECDSK00.8xv").exists())

    def test_metadata_destination_cannot_overwrite_profile(self):
        profile = self.make_profile(path=self.directory / "ECDINFO.8xv")
        original = profile.read_bytes()
        self.assert_refused_without_writes(
            lambda: optimizer.convert([self.disk], self.directory, "dsk", "unused", profile), "overwrite a source")
        self.assertEqual(profile.read_bytes(), original)
        self.assertFalse((self.directory / "ECDSK00.8xv").exists())
        self.assertFalse((self.directory / "optimization-dsk.json").exists())

    def test_metadata_destination_cannot_overwrite_replacement(self):
        replacement = self.directory / "ECDINFO.8xv"
        profile = self.make_profile(replacement=replacement)
        original = replacement.read_bytes()
        self.assert_refused_without_writes(
            lambda: optimizer.convert([self.disk], self.directory, "dsk", "unused", profile), "overwrite a source")
        self.assertEqual(replacement.read_bytes(), original)
        self.assertFalse((self.directory / "ECDSK00.8xv").exists())

    def test_metadata_hardlink_to_input_is_rejected_by_both_converters(self):
        for optimized in (False, True):
            output = self.directory / f"hardlink-input-{optimized}"
            output.mkdir()
            alias = output / "ECDINFO.8xv"
            self.hardlink_or_skip(alias, self.disk)
            with self.subTest(optimized=optimized):
                if optimized:
                    action = lambda: optimizer.convert([self.disk], output, "dsk", "unused")
                else:
                    action = lambda: d64.convert(self.disk, output, "unused")
                self.assert_refused_without_writes(action, "overwrite a source")
                self.assertEqual(list(output.iterdir()), [alias])
                self.assertEqual(alias.read_bytes(), self.image)
                self.assertTrue(alias.samefile(self.disk))

    def test_metadata_hardlinks_to_profile_and_replacement_are_rejected(self):
        replacement = self.directory / "replacement.prg"
        profile = self.make_profile(replacement=replacement)
        for source in (profile, replacement):
            output = self.directory / ("hardlink-" + source.stem)
            output.mkdir()
            alias = output / "ECDINFO.8xv"
            original = source.read_bytes()
            self.hardlink_or_skip(alias, source)
            with self.subTest(source=source.name):
                self.assert_refused_without_writes(
                    lambda: optimizer.convert([self.disk], output, "dsk", "unused", profile), "overwrite a source")
                self.assertEqual(list(output.iterdir()), [alias])
                self.assertEqual(alias.read_bytes(), original)
                self.assertTrue(alias.samefile(source))

    def test_metadata_hardlinks_to_other_outputs_are_rejected_before_writes(self):
        for optimized, name in ((False, "ECDSK00.8xv"), (True, "ECDSK00.8xv"),
                                (True, "optimization-dsk.json")):
            output = self.directory / f"hardlink-output-{optimized}-{name}"
            output.mkdir()
            other = output / name
            other.write_bytes(b"preexisting")
            alias = output / "ECDINFO.8xv"
            self.hardlink_or_skip(alias, other)
            with self.subTest(optimized=optimized, name=name):
                if optimized:
                    action = lambda: optimizer.convert([self.disk], output, "dsk", "unused")
                else:
                    action = lambda: d64.convert(self.disk, output, "unused")
                self.assert_refused_without_writes(action, "another output")
                self.assertEqual({path.name for path in output.iterdir()}, {name, "ECDINFO.8xv"})
                self.assertEqual(other.read_bytes(), b"preexisting")
                self.assertTrue(alias.samefile(other))


class MetadataFormatTests(unittest.TestCase):
    def setUp(self):
        self.payload = payload_for(count=2)
        self.types = b"\x82\xc2"
        self.encoded = metadata.build_metadata(self.payload, self.types)

    def test_exact_wire_format_and_both_ieee_checksums(self):
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)
        expected = b"ECFI\x01\x02\x00\x00"
        expected += len(self.payload).to_bytes(4, "little")
        expected += zlib.crc32(self.payload, 0).to_bytes(4, "little") + self.types
        expected += zlib.crc32(expected, 0).to_bytes(4, "little")
        self.assertEqual(metadata.HEADER.size, 16)
        self.assertEqual(len(self.encoded), 22)
        self.assertEqual(self.encoded, expected)
        self.assertEqual(metadata.validate_metadata(self.encoded, self.payload), self.types)

    def test_format_accepts_1_through_255_entries(self):
        for count in (1, 144, 255):
            with self.subTest(count=count):
                payload = payload_for(count=count)
                file_types = b"\xc2" * count
                encoded = metadata.build_metadata(payload, file_types)
                self.assertEqual(len(encoded), 20 + count)
                self.assertEqual(metadata.validate_metadata(encoded, payload), file_types)

    def test_rejects_empty_excess_mismatched_counts_and_short_payload(self):
        for payload, file_types in ((b"", b""), (b"\x00", b"\x82"),
                                    (self.payload, b"\x82"), (b"\x01", b"\x82"),
                                    (self.payload, b"\x82" * 256)):
            with self.subTest(size=len(payload), count=len(file_types)), self.assertRaises(ValueError):
                metadata.build_metadata(payload, file_types)

    def test_every_type_byte_uses_closed_prg_mask(self):
        payload = payload_for()
        for file_type in range(256):
            with self.subTest(file_type=file_type):
                if (file_type & 0x87) == 0x82:
                    encoded = metadata.build_metadata(payload, bytes([file_type]))
                    self.assertEqual(metadata.validate_metadata(encoded, payload), bytes([file_type]))
                else:
                    with self.assertRaisesRegex(ValueError, "closed PRGs"):
                        metadata.build_metadata(payload, bytes([file_type]))

    def test_rejects_truncation_and_trailing_bytes(self):
        for size in range(len(self.encoded)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                metadata.validate_metadata(self.encoded[:size], self.payload)
        with self.assertRaisesRegex(ValueError, "length"):
            metadata.validate_metadata(self.encoded + b"\x00", self.payload)

    def test_rejects_header_and_type_errors_even_with_valid_checksum(self):
        for offset, value in ((0, 0), (4, 2), (5, 0), (5, 1), (6, 1), (7, 1), (16, 0x81)):
            damaged = bytearray(self.encoded[:-4])
            damaged[offset] = value
            encoded = bytes(damaged) + struct.pack("<I", zlib.crc32(damaged))
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                metadata.validate_metadata(encoded, self.payload)

    def test_metadata_checksum_covers_header_types_and_trailer(self):
        for offset in range(len(self.encoded)):
            damaged = bytearray(self.encoded)
            damaged[offset] ^= 0x40
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                metadata.validate_metadata(bytes(damaged), self.payload)

    def test_rejects_different_payload_length(self):
        with self.assertRaisesRegex(ValueError, "payload length"):
            metadata.validate_metadata(self.encoded, self.payload + b"\x00")

    def test_whole_payload_crc_detects_later_chunk_change_with_same_sum(self):
        payload = payload_for(b"\x01\x08" + bytes(range(256)) * 300)
        encoded = metadata.build_metadata(payload, b"\xc2")
        changed = bytearray(payload)
        position = rta.CHUNK_SIZE * 2 + 100
        changed[position] += 1
        changed[position + 1] -= 1
        self.assertEqual(payload[:rta.CHUNK_SIZE], changed[:rta.CHUNK_SIZE])
        self.assertEqual(sum(payload) & 0xFFFF, sum(changed) & 0xFFFF)
        self.assertEqual(len(payload), len(changed))
        with self.assertRaisesRegex(ValueError, "payload CRC32"):
            metadata.validate_metadata(encoded, bytes(changed))


if __name__ == "__main__":
    unittest.main()