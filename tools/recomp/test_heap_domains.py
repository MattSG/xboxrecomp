"""Regression guard for split guest-VA and kernel allocation domains."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_nt_virtual_alloc_does_not_use_kernel_heap_cursor():
    bridge = (ROOT / "src/kernel/kernel_bridge.c").read_text(encoding="utf-8")
    body = bridge.split("static void bridge_NtAllocateVirtualMemory(void)", 1)[1]
    body = body.split("static void bridge_NtFreeVirtualMemory(void)", 1)[0]
    assert "xbox_VirtualAlloc(size, 4096)" in body
    assert "xbox_HeapAlloc(size, 4096)" not in body


def test_two_cursors_grow_toward_each_other_with_collision_guards():
    source = (ROOT / "src/kernel/xbox_memory_layout.c").read_text(encoding="utf-8")
    assert "static uint32_t g_virtual_next = XBOX_HEAP_BASE;" in source
    assert "static uint32_t g_heap_next = XBOX_HEAP_BASE + XBOX_HEAP_SIZE;" in source
    assert "result < g_virtual_next" in source
    assert "result + (uint64_t)size > (uint64_t)g_heap_next" in source


if __name__ == "__main__":
    test_nt_virtual_alloc_does_not_use_kernel_heap_cursor()
    test_two_cursors_grow_toward_each_other_with_collision_guards()
    print("all heap-domain checks passed")
