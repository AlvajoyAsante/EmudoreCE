from contextlib import redirect_stdout
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from py65.devices.mpu6502 import MPU

import d64_to_appvar as d64
import disk_optimizations as optimizations
import optimize_appvar as converter


class TrackedMemory(list):
    def __init__(self):
        super().__init__([0] * 65536)
        self.bank_writes = 0

    def __setitem__(self, address, value):
        if address == 1:
            self.bank_writes += 1
        super().__setitem__(address, value)


def run_writer(code, address, value, counter, flags):
    memory = TrackedMemory()
    memory[1] = 0x38
    memory[0x0400] = counter
    memory[0x01B0:0x01B0 + len(code)] = code
    memory[0x01B1] = address & 255
    memory[0x01B2] = address >> 8
    memory[0x01F0:0x01F2] = [0xFF, 0x02]
    memory.bank_writes = 0
    machine = MPU(memory=memory, pc=0x01B0)
    machine.a = value
    machine.x = 0x93
    machine.y = 0x57
    machine.p = flags
    machine.sp = 0xEF
    for _ in range(32):
        if machine.pc == 0x0300:
            return machine, memory
        machine.step()
    raise AssertionError("writer did not return")


class WriterEquivalenceTests(unittest.TestCase):
    def test_original_and_fast_writer_match_except_code_and_timing(self):
        for address in (0x080B, 0x08FF, 0x3FFF, 0xD000, 0xFFFF):
            for value in (0, 0x4D, 0x80, 0xFF):
                for counter in (0, 0x7F, 0xFF):
                    for flags in (0x24, 0x6D, 0xE7):
                        with self.subTest(address=address, value=value, counter=counter, flags=flags):
                            original, original_memory = run_writer(
                                optimizations.MRCROSS_ORIGINAL_WRITER, address, value, counter, flags
                            )
                            changed, changed_memory = run_writer(
                                optimizations.MRCROSS_FAST_WRITER, address, value, counter, flags
                            )
                            for register in ("a", "x", "y", "p", "sp", "pc"):
                                self.assertEqual(getattr(changed, register), getattr(original, register))
                            self.assertEqual(original_memory.bank_writes, 2)
                            self.assertEqual(changed_memory.bank_writes, 0)
                            self.assertEqual(original.processorCycles - changed.processorCycles, 10)
                            original_memory[0x01B3:0x01C3] = [0] * 16
                            changed_memory[0x01B3:0x01C3] = [0] * 16
                            self.assertEqual(changed_memory, original_memory)


def synthetic_program(stream=None):
    if stream is None:
        stream = bytes([0x61]) * 180 + bytes.fromhex("4d 04 a5 4d 02 4d 00 00 00")
    program = bytearray(optimizations.MRCROSS_PACKED - optimizations.MRCROSS_LOAD + 2)
    program[:2] = optimizations.MRCROSS_LOAD.to_bytes(2, "little")
    for address, expected in optimizations.MRCROSS_SIGNATURES:
        offset = address - optimizations.MRCROSS_LOAD + 2
        program[offset:offset + len(expected)] = expected
    program.extend(stream)

    def put_word(address, value):
        offset = address - optimizations.MRCROSS_LOAD + 2
        program[offset:offset + 2] = value.to_bytes(2, "little")

    put_word(0x082C, optimizations.MRCROSS_LOAD + len(program) - 3)
    put_word(0x0854, (len(stream) & 255) | (((len(stream) - 1) // 256 + 1) << 8))
    put_word(0x0857, (0x00A0 - len(stream)) & 0xFFFF)
    put_word(0x08B4, 0xC000)
    return bytes(program)


def run_bootstrap(program, seed=0):
    memory = TrackedMemory()
    memory[:] = [(address * 17 + seed) & 255 for address in range(65536)]
    base = int.from_bytes(program[:2], "little")
    memory[base:base + len(program) - 2] = program[2:]
    machine = MPU(memory=memory, pc=optimizations.MRCROSS_ENTRY)
    machine.a, machine.x, machine.y, machine.p = 0x31, 0x52, seed, 0x24
    writer_calls = 0
    continuation_offset = 0x08B4 - base + 2
    continuation = int.from_bytes(program[continuation_offset:continuation_offset + 2], "little")
    for instructions in range(2000000):
        if machine.pc == continuation:
            return machine, memory, writer_calls, instructions
        writer_calls += machine.pc == 0x01B0
        machine.step()
    raise AssertionError(f"bootstrap did not reach ${continuation:04x}; stopped at ${machine.pc:04x}")


class PackerRecognitionTests(unittest.TestCase):
    def test_recognizes_packer_without_game_names_or_hashes(self):
        original = synthetic_program()
        changed, info = optimizations.optimize_mrcross(original)
        self.assertIsNotNone(info)
        self.assertNotEqual(original, changed)
        self.assertEqual(len(changed), len(original))
        self.assertEqual(changed[:2], original[:2])
        self.assertEqual(info["decoded_bytes"], 185)
        self.assertEqual(info["bank_writes_removed"], 370)
        self.assertEqual(info["guest_cycles_saved"], 1850)
        offset = optimizations.MRCROSS_WRITER - optimizations.MRCROSS_LOAD + 2
        self.assertEqual(changed[:offset], original[:offset])
        self.assertEqual(changed[offset + 19:], original[offset + 19:])

    def test_optimized_and_unknown_programs_are_left_alone(self):
        original = synthetic_program()
        changed, _ = optimizations.optimize_mrcross(original)
        for program in (b"", b"\x01", b"\x01\x08unknown", changed):
            self.assertEqual(optimizations.optimize_mrcross(program), (program, None))

    def test_rejects_every_modified_instruction_signature(self):
        original = synthetic_program()
        for address, expected in optimizations.MRCROSS_SIGNATURES:
            for byte_index in range(len(expected)):
                program = bytearray(original)
                offset = address - optimizations.MRCROSS_LOAD + 2 + byte_index
                program[offset] ^= 1
                with self.subTest(address=address + byte_index):
                    self.assertEqual(optimizations.optimize_mrcross(bytes(program)), (bytes(program), None))

    def test_rejects_bad_relocation_size_and_read_pointer(self):
        original = synthetic_program()
        for address in (0x082C, 0x082D, 0x0854, 0x0855, 0x0857, 0x0858):
            program = bytearray(original)
            program[address - optimizations.MRCROSS_LOAD + 2] ^= 1
            with self.subTest(address=address):
                self.assertEqual(optimizations.optimize_mrcross(bytes(program)), (bytes(program), None))
        self.assertEqual(optimizations.optimize_mrcross(original[:-1]), (original[:-1], None))

    def test_rejects_truncated_and_unsafe_streams(self):
        prefixes = (
            b"\x4d", b"\x4d\x03", b"\x4d\x00\x10",
            bytes.fromhex("4d 00 00 01 61"),
            bytes.fromhex("4d 00 f0 ff 4d ff 61"),
            bytes.fromhex("4d 00 f0 ff 61"),
        )
        for prefix in prefixes:
            if prefix in (b"\x4d", b"\x4d\x03", b"\x4d\x00\x10"):
                stream = bytes([0x61]) * 180 + prefix
            else:
                stream = prefix + bytes([0x61]) * 180 + bytes.fromhex("4d 00 00 00")
            program = synthetic_program(stream)
            with self.subTest(prefix=prefix):
                self.assertEqual(optimizations.optimize_mrcross(program), (program, None))

    def test_final_byte_wrap_is_safe_but_following_zero_page_write_is_rejected(self):
        terminal = bytes.fromhex("4d 00 ff ff 61 4d 00 00 00").ljust(164, b"\x61")
        unsafe = bytes.fromhex("4d 00 ff ff 61 62 4d 00 00 00").ljust(164, b"\x61")
        original = synthetic_program(terminal)
        changed, info = optimizations.optimize_mrcross(original)
        self.assertIsNotNone(info)
        self.assertEqual(info["decoded_bytes"], 1)
        reference, memory, _, _ = run_bootstrap(original)
        optimized, optimized_memory, _, _ = run_bootstrap(changed)
        for register in ("a", "x", "y", "p", "sp", "pc"):
            self.assertEqual(getattr(optimized, register), getattr(reference, register))
        self.assertEqual(memory[0xFFFF], 0x61)
        self.assertEqual(optimized_memory[0xFFFF], memory[0xFFFF])
        self.assertEqual(optimized_memory[:0x0100], memory[:0x0100])
        rejected = synthetic_program(unsafe)
        self.assertEqual(optimizations.optimize_mrcross(rejected), (rejected, None))

    def test_complete_synthetic_bootstraps_preserve_state(self):
        original = synthetic_program()
        changed, info = optimizations.optimize_mrcross(original)
        self.assertIsNotNone(info)
        for seed in (0, 57, 255):
            reference, memory, writer_calls, _ = run_bootstrap(original, seed)
            optimized, optimized_memory, optimized_calls, _ = run_bootstrap(changed, seed)
            for register in ("a", "x", "y", "p", "sp", "pc"):
                self.assertEqual(getattr(optimized, register), getattr(reference, register))
            self.assertEqual(writer_calls, optimized_calls)
            self.assertEqual(writer_calls, info["decoded_bytes"])
            self.assertEqual(reference.processorCycles - optimized.processorCycles, info["guest_cycles_saved"])
            self.assertEqual(memory.bank_writes - optimized_memory.bank_writes, info["bank_writes_removed"])
            memory[0x01B3:0x01C3] = optimized_memory[0x01B3:0x01C3]
            source = optimizations.MRCROSS_WRITER + 3
            memory[source:source + 16] = optimized_memory[source:source + 16]
            self.assertEqual(memory, optimized_memory)


class SelectedDiskTests(unittest.TestCase):
    def test_final_blow_full_bootstrap(self):
        path = Path(__file__).resolve().parents[1] / "tests" / "FINALBLW.D64"
        image = path.read_bytes()
        _, track, sector = d64.read_directory(image)[0]
        original = d64.extract_file(image, track, sector)
        changed, info = optimizations.optimize_mrcross(original)
        self.assertIsNotNone(info)
        reference, memory, writer_calls, instructions = run_bootstrap(original)
        optimized, optimized_memory, optimized_calls, optimized_instructions = run_bootstrap(changed)
        for register in ("a", "x", "y", "p", "sp", "pc"):
            self.assertEqual(getattr(optimized, register), getattr(reference, register))
        self.assertEqual(reference.pc, 0xC000)
        self.assertEqual(writer_calls, 48426)
        self.assertEqual(optimized_calls, writer_calls)
        self.assertEqual(info["decoded_bytes"], writer_calls)
        self.assertEqual(reference.processorCycles - optimized.processorCycles, 484260)
        self.assertEqual(instructions - optimized_instructions, 96852)
        self.assertEqual(memory.bank_writes - optimized_memory.bank_writes, 96852)
        memory[0x01B3:0x01C3] = optimized_memory[0x01B3:0x01C3]
        self.assertEqual(memory, optimized_memory)
        self.assertEqual(path.read_bytes(), image)


class ConverterIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "unrelated-title.prg"
        self.original = synthetic_program()
        self.source.write_bytes(self.original)

    def test_default_disk_conversion_applies_real_code_change(self):
        unknown = self.directory / "unknown.prg"
        unknown.write_bytes(b"\x01\x08unknown build")
        payload, report = converter.prepare_conversion([self.source, unknown])
        files = converter.read_payload(payload)
        expected, _ = optimizations.optimize_mrcross(self.original)
        self.assertEqual(files[0].data, expected)
        self.assertEqual(files[1].data, unknown.read_bytes())
        self.assertEqual(report["optimization_status"], "code-transformed")
        self.assertEqual(report["files_changed"], 1)
        self.assertEqual(report["files_changed_automatically"], 1)
        self.assertEqual(report["files_changed_by_profile"], 0)
        self.assertEqual(report["files"][0]["automatic_optimizations"][0]["guest_cycles_saved"], 1850)
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_selected_d64_has_a_changed_program(self):
        path = Path(__file__).resolve().parents[1] / "tests" / "FINALBLW.D64"
        normal = d64.build_payload(path.read_bytes())
        payload, report = converter.prepare_conversion([path])
        self.assertNotEqual(payload, normal)
        self.assertEqual(len(payload), len(normal))
        self.assertEqual(report["files_changed_automatically"], 1)
        self.assertEqual(report["files_changed_by_profile"], 0)
        normal_files = converter.read_payload(normal)
        optimized_files = converter.read_payload(payload)
        self.assertEqual([entry.name for entry in optimized_files], [entry.name for entry in normal_files])
        self.assertEqual(optimized_files[0].data[:2], normal_files[0].data[:2])
        self.assertEqual(report["files"][0]["automatic_optimizations"][0]["bank_writes_removed"], 96852)

    def test_firmware_and_cartridge_paths_do_not_apply_disk_pass(self):
        payload, report = converter.prepare_conversion([self.source], "cart")
        self.assertEqual(payload, self.original)
        self.assertEqual(report["optimization_status"], "unchanged")
        self.assertEqual(report["files_changed"], 0)

    def test_explicit_profile_takes_precedence_without_implicit_chaining(self):
        changed = self.original[:-1] + b"\x01"
        profile = self.directory / "profile.json"
        profile.write_text(json.dumps({
            "version": 1, "name": "Synthetic test", "rules": [{
                "id": "test", "description": "Synthetic profile precedence test",
                "input_sha256": converter.sha256(self.original),
                "output_sha256": converter.sha256(changed),
                "patches": [{"offset": len(changed) - 1, "expected": "00", "replacement": "01"}],
            }]
        }), encoding="utf-8")
        payload, report = converter.prepare_conversion([self.source], profile=profile)
        self.assertEqual(converter.read_payload(payload)[0].data, changed)
        self.assertEqual(report["files_changed_by_profile"], 1)
        self.assertEqual(report["files_changed_automatically"], 0)

    def test_require_change_preserves_existing_output_when_no_pass_matches(self):
        self.source.write_bytes(b"\x01\x08unknown build")
        output = self.directory / "output"
        output.mkdir()
        existing = output / "ECDSK00.8xv"
        existing.write_bytes(b"existing package")
        with mock.patch.object(converter.rta, "write_chunks") as write:
            with self.assertRaisesRegex(ValueError, "no supported code optimization matched"):
                converter.convert([self.source], output, "dsk", "unused", require_code_change=True)
        write.assert_not_called()
        self.assertEqual(existing.read_bytes(), b"existing package")

    def test_cli_dry_run_reports_code_change_without_convbin(self):
        output = io.StringIO()
        arguments = ["optimize_appvar.py", str(self.source), "--dry-run", "--require-code-change"]
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(converter.rta, "find_convbin") as find:
            with redirect_stdout(output):
                self.assertEqual(converter.main(), 0)
        find.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())["files_changed_automatically"], 1)

    def test_plain_conversion_marks_unchanged_code_explicitly(self):
        self.source.write_bytes(b"\x01\x08unknown build")
        output_dir = self.directory / "output"
        output_dir.mkdir()
        captured = io.StringIO()
        with mock.patch.object(converter.rta, "write_chunks", return_value=[]), redirect_stdout(captured):
            converter.convert([self.source], output_dir, "dsk", "unused")
        self.assertIn("NO CODE OPTIMIZATION APPLIED", captured.getvalue())


if __name__ == "__main__":
    unittest.main()