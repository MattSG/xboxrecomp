"""Synthetic address-map detection independent of any BC title or module."""
from types import SimpleNamespace

import numpy as np
import pytest

from tools.fusion.module import FusionModule


@pytest.mark.parametrize("guest_base", [0x10000, 0x600000, 0xF0000000])
@pytest.mark.parametrize("phase", [0, 1])
@pytest.mark.parametrize("trailing_guest", [False, True])
def test_address_map_has_no_corpus_image_ceiling(guest_base, phase, trailing_guest):
    # Exceed the detector's existing confidence threshold. Host deltas are
    # deliberately larger than guest deltas so only one column is a candidate.
    count = 50002
    guest = guest_base + np.arange(count, dtype=np.uint32) * 4
    host = 0x10000000 + np.arange(count, dtype=np.uint32) * 64
    pairs = np.empty(count * 2, dtype=np.uint32)
    pairs[::2], pairs[1::2] = guest, host
    raw = bytes(4 * phase) + pairs.tobytes()
    if trailing_guest:
        raw += (int(guest[-1]) + 4).to_bytes(4, "little")
    module = FusionModule.__new__(FusionModule)
    module.img = raw
    module.pe = SimpleNamespace(sections=[SimpleNamespace(
        Name=b".rdata\0\0", VirtualAddress=0, Misc_VirtualSize=len(raw))])

    actual_guest, actual_host = module.map_pairs()
    np.testing.assert_array_equal(actual_guest, guest)
    np.testing.assert_array_equal(actual_host, host)
    np.testing.assert_array_equal(module.map_entry_points(), guest)
    assert module.host_of(int(guest[-1])) == int(host[-1])
    assert module.host_of(int(guest[0]) + 1) is None
    assert module.map_entry_points(max_delta=3).size == 0
