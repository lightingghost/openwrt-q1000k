# SPDX-License-Identifier: GPL-2.0-only
"""Read FIT/FDT bytes without executing image contents."""
import struct


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
