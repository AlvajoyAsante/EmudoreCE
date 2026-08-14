#!/usr/bin/env python3
"""Minimal 6502 disassembler for analyzing D64 game loaders."""

from pathlib import Path
import sys

SPT = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5


def off(t, s):
    return (sum(SPT[: t - 1]) + s) * 256


def extract(data, t, s):
    out = bytearray()
    while t:
        b = data[off(t, s):][:256]
        nt, ns = b[0], b[1]
        if nt == 0:
            out += b[2:ns + 1]
            break
        out += b[2:256]
        t, s = nt, ns
    return bytes(out)


# addressing modes: (size, format)
IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL = range(13)
SIZES = {IMP: 1, ACC: 1, IMM: 2, ZP: 2, ZPX: 2, ZPY: 2, ABS: 3, ABX: 3,
         ABY: 3, IND: 3, IZX: 2, IZY: 2, REL: 2}

OPS = {}


def _fill():
    def put(base, name, mode):
        OPS[base] = (name, mode)

    # standard opcode grid
    table = [
        (0x00, "BRK", IMP), (0x01, "ORA", IZX), (0x05, "ORA", ZP),
        (0x06, "ASL", ZP), (0x08, "PHP", IMP), (0x09, "ORA", IMM),
        (0x0A, "ASL", ACC), (0x0D, "ORA", ABS), (0x0E, "ASL", ABS),
        (0x10, "BPL", REL), (0x11, "ORA", IZY), (0x15, "ORA", ZPX),
        (0x16, "ASL", ZPX), (0x18, "CLC", IMP), (0x19, "ORA", ABY),
        (0x1D, "ORA", ABX), (0x1E, "ASL", ABX),
        (0x20, "JSR", ABS), (0x21, "AND", IZX), (0x24, "BIT", ZP),
        (0x25, "AND", ZP), (0x26, "ROL", ZP), (0x28, "PLP", IMP),
        (0x29, "AND", IMM), (0x2A, "ROL", ACC), (0x2C, "BIT", ABS),
        (0x2D, "AND", ABS), (0x2E, "ROL", ABS),
        (0x30, "BMI", REL), (0x31, "AND", IZY), (0x35, "AND", ZPX),
        (0x36, "ROL", ZPX), (0x38, "SEC", IMP), (0x39, "AND", ABY),
        (0x3D, "AND", ABX), (0x3E, "ROL", ABX),
        (0x40, "RTI", IMP), (0x41, "EOR", IZX), (0x45, "EOR", ZP),
        (0x46, "LSR", ZP), (0x48, "PHA", IMP), (0x49, "EOR", IMM),
        (0x4A, "LSR", ACC), (0x4C, "JMP", ABS), (0x4D, "EOR", ABS),
        (0x4E, "LSR", ABS),
        (0x50, "BVC", REL), (0x51, "EOR", IZY), (0x55, "EOR", ZPX),
        (0x56, "LSR", ZPX), (0x58, "CLI", IMP), (0x59, "EOR", ABY),
        (0x5D, "EOR", ABX), (0x5E, "LSR", ABX),
        (0x60, "RTS", IMP), (0x61, "ADC", IZX), (0x65, "ADC", ZP),
        (0x66, "ROR", ZP), (0x68, "PLA", IMP), (0x69, "ADC", IMM),
        (0x6A, "ROR", ACC), (0x6C, "JMP", IND), (0x6D, "ADC", ABS),
        (0x6E, "ROR", ABS),
        (0x70, "BVS", REL), (0x71, "ADC", IZY), (0x75, "ADC", ZPX),
        (0x76, "ROR", ZPX), (0x78, "SEI", IMP), (0x79, "ADC", ABY),
        (0x7D, "ADC", ABX), (0x7E, "ROR", ABX),
        (0x81, "STA", IZX), (0x84, "STY", ZP), (0x85, "STA", ZP),
        (0x86, "STX", ZP), (0x88, "DEY", IMP), (0x8A, "TXA", IMP),
        (0x8C, "STY", ABS), (0x8D, "STA", ABS), (0x8E, "STX", ABS),
        (0x90, "BCC", REL), (0x91, "STA", IZY), (0x94, "STY", ZPX),
        (0x95, "STA", ZPX), (0x96, "STX", ZPY), (0x98, "TYA", IMP),
        (0x99, "STA", ABY), (0x9A, "TXS", IMP), (0x9D, "STA", ABX),
        (0xA0, "LDY", IMM), (0xA1, "LDA", IZX), (0xA2, "LDX", IMM),
        (0xA4, "LDY", ZP), (0xA5, "LDA", ZP), (0xA6, "LDX", ZP),
        (0xA8, "TAY", IMP), (0xA9, "LDA", IMM), (0xAA, "TAX", IMP),
        (0xAC, "LDY", ABS), (0xAD, "LDA", ABS), (0xAE, "LDX", ABS),
        (0xB0, "BCS", REL), (0xB1, "LDA", IZY), (0xB4, "LDY", ZPX),
        (0xB5, "LDA", ZPX), (0xB6, "LDX", ZPY), (0xB8, "CLV", IMP),
        (0xB9, "LDA", ABY), (0xBA, "TSX", IMP), (0xBC, "LDY", ABX),
        (0xBD, "LDA", ABX), (0xBE, "LDX", ABY),
        (0xC0, "CPY", IMM), (0xC1, "CMP", IZX), (0xC4, "CPY", ZP),
        (0xC5, "CMP", ZP), (0xC6, "DEC", ZP), (0xC8, "INY", IMP),
        (0xC9, "CMP", IMM), (0xCA, "DEX", IMP), (0xCC, "CPY", ABS),
        (0xCD, "CMP", ABS), (0xCE, "DEC", ABS),
        (0xD0, "BNE", REL), (0xD1, "CMP", IZY), (0xD5, "CMP", ZPX),
        (0xD6, "DEC", ZPX), (0xD8, "CLD", IMP), (0xD9, "CMP", ABY),
        (0xDD, "CMP", ABX), (0xDE, "DEC", ABX),
        (0xE0, "CPX", IMM), (0xE1, "SBC", IZX), (0xE4, "CPX", ZP),
        (0xE5, "SBC", ZP), (0xE6, "INC", ZP), (0xE8, "INX", IMP),
        (0xE9, "SBC", IMM), (0xEA, "NOP", IMP), (0xEC, "CPX", ABS),
        (0xED, "SBC", ABS), (0xEE, "INC", ABS),
        (0xF0, "BEQ", REL), (0xF1, "SBC", IZY), (0xF5, "SBC", ZPX),
        (0xF6, "INC", ZPX), (0xF8, "SED", IMP), (0xF9, "SBC", ABY),
        (0xFD, "SBC", ABX), (0xFE, "INC", ABX),
    ]
    for op, name, mode in table:
        put(op, name, mode)


_fill()


def disasm(mem, base, start, end):
    pc = start
    while pc < end:
        i = pc - base
        op = mem[i]
        if op not in OPS:
            print(f"${pc:04x}: {op:02x}          ???")
            pc += 1
            continue
        name, mode = OPS[op]
        size = SIZES[mode]
        raw = mem[i:i + size].hex(" ")
        if mode == IMP:
            arg = ""
        elif mode == ACC:
            arg = "A"
        elif mode == IMM:
            arg = f"#${mem[i+1]:02x}"
        elif mode == ZP:
            arg = f"${mem[i+1]:02x}"
        elif mode == ZPX:
            arg = f"${mem[i+1]:02x},X"
        elif mode == ZPY:
            arg = f"${mem[i+1]:02x},Y"
        elif mode == REL:
            arg = f"${pc + 2 + (mem[i+1] ^ 0x80) - 0x80:04x}"
        else:
            w = mem[i + 1] | (mem[i + 2] << 8)
            arg = {ABS: f"${w:04x}", ABX: f"${w:04x},X", ABY: f"${w:04x},Y",
                   IND: f"(${w:04x})", IZX: f"(${w:02x},X)",
                   IZY: f"(${w:02x}),Y"}[mode]
        print(f"${pc:04x}: {raw:<9} {name} {arg}")
        pc += size


if __name__ == "__main__":
    data = Path("tests/Rally Driver.d64").read_bytes()
    part1 = extract(data, 17, 1)[2:]
    base = 0x18D0
    for spec in sys.argv[1:] or ["1a4e-1b7f"]:
        lo, hi = (int(x, 16) for x in spec.split("-"))
        print(f"\n===== ${lo:04x}-${hi:04x} =====")
        disasm(part1, base, lo, hi)
