# SPDX-License-Identifier: GPL-2.0-only
"""Read FIT/FDT bytes without executing image contents."""
import struct


def elf_defined_symbols(data):
    """Read symbols from a little-endian AArch64 relocatable kernel module."""
    return _elf_symbols(data, defined=True)


def elf_undefined_symbols(data):
    """Read imported symbols, including calls from inlined implementation code."""
    return _elf_symbols(data, defined=False)


def _elf_symbols(data, *, defined):
    assert len(data) >= 64 and data[:6] == b'\x7fELF\x02\x01'
    assert struct.unpack_from('<HH', data, 16) == (1, 183)
    offset = struct.unpack_from('<Q', data, 40)[0]
    stride, count = struct.unpack_from('<HH', data, 58)
    assert stride == 64 and count and offset + count * stride <= len(data)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * stride)
                for i in range(count)]
    symbols = set()
    found = False
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        found = True
        start, size, link, entry_size = section[4], section[5], section[6], section[9]
        assert entry_size == 24 and size % entry_size == 0 and start + size <= len(data)
        assert link < count and sections[link][1] == 3  # SHT_STRTAB
        strings = sections[link]
        assert strings[4] + strings[5] <= len(data)
        names = data[strings[4]:strings[4] + strings[5]]
        for entry in range(start, start + size, entry_size):
            name = struct.unpack_from('<I', data, entry)[0]
            index = struct.unpack_from('<H', data, entry + 6)[0]
            assert name < len(names)
            if bool(index) == defined:  # SHN_UNDEF is zero.
                symbols.add(names[name:names.index(0, name)].decode('ascii'))
    assert found
    return symbols


def u32(data, offset=0):
    return struct.unpack_from('>I', data, offset)[0]


def fdt(data):
    magic, size, tokens, strings, _, version, _, _, strings_size, tokens_size = struct.unpack_from('>10I', data)
    assert magic == 0xd00dfeed and version == 17 and size <= len(data)
    assert tokens + tokens_size <= size and strings + strings_size <= size
    names = data[strings:strings + strings_size]
    end = tokens + tokens_size
    nodes, stack = {}, []
    while tokens < end:
        token = u32(data, tokens)
        tokens += 4
        if token == 1:
            stop = data.index(0, tokens, end)
            stack.append(data[tokens:stop].decode('ascii'))
            path = '/'.join(stack) or '/'
            assert path not in nodes
            nodes[path] = {}
            tokens = (stop + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name = struct.unpack_from('>II', data, tokens)
            tokens += 8
            assert tokens + length <= end and name < len(names)
            key = names[name:names.index(0, name)].decode('ascii')
            path = '/'.join(stack) or '/'
            assert key not in nodes[path]
            nodes[path][key] = data[tokens:tokens + length]
            tokens = (tokens + length + 3) & ~3
        elif token == 9:
            assert not stack
            return nodes, size
        else:
            assert token == 4
    raise AssertionError('FDT lacks an end token')
