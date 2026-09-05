from pathlib import Path


SOURCE = (Path(__file__).parents[2] / "src/kernel/kernel_bridge.c").read_text()


def body(name: str) -> str:
    start = SOURCE.index(name)
    start = SOURCE.index("{", start)
    depth = 0
    for end in range(start, len(SOURCE)):
        if SOURCE[end] == "{":
            depth += 1
        elif SOURCE[end] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start : end + 1]
    raise AssertionError(f"unterminated {name}")


def test_xe_section_ordinals_are_functions_with_one_argument():
    data = body("kernel_data_va_for_ordinal")
    args = body("static int stdcall_args_for_ordinal(")
    dispatch = body("static bridge_func_t bridge_for_ordinal(")

    assert "case 327:" not in data
    assert "case 328:" not in data
    assert "case 327: return  4;" in args
    assert "case 328: return  4;" in args
    assert "case 327: return bridge_XeLoadSection;" in dispatch
    assert "case 328: return bridge_XeUnloadSection;" in dispatch


def test_ex_query_pool_block_size_uses_xbox_ordinal_23():
    args = body("static int stdcall_args_for_ordinal(")
    dispatch = body("static bridge_func_t bridge_for_ordinal(")

    assert "case  23: return  4;  /* ExQueryPoolBlockSize(1) */" in args
    assert "case  23: return  0;  /* Unknown_23(void) */" not in args
    assert "case  24: return  4;  /* ExQueryPoolBlockSize(1) */" not in args
    assert "case  23: return bridge_ExQueryPoolBlockSize;" in dispatch
    assert "case  24: return bridge_ExQueryPoolBlockSize;" not in dispatch


def test_bink_worker_thread_ordinals_and_scheduler_are_live():
    args = body("static int stdcall_args_for_ordinal(")
    dispatch = body("static bridge_func_t bridge_for_ordinal(")

    assert "case 224: return  8;  /* NtResumeThread(2) */" in args
    assert "case 234: return 16;  /* NtWaitForSingleObjectEx(4) */" in args
    assert "case 224: return bridge_NtResumeThread;" in dispatch
    assert "case 225: return bridge_NtSetEvent;" in dispatch
    assert "case 234: return bridge_NtWaitForSingleObjectEx;" in dispatch
    assert "worker_state_t g_workers[MAX_GUEST_WORKERS]" in SOURCE
    assert "uint32_t create_suspended = STACK_ARG(7);" in SOURCE
    assert SOURCE.count("!guest_stack_contains_esp(g_esp)") == 2
    stack_guard = body("static int guest_stack_contains_esp(")
    assert "XBOX_WORKER_STACK_SIZE" in stack_guard
    assert "w->stack_top" in stack_guard
    create = body("static void bridge_PsCreateSystemThreadEx(")
    finish = body("static void WINAPI worker_fiber_main(")
    reference = body("static void bridge_ObReferenceObjectByHandle(")
    assert "w->object = xbox_HeapAlloc(0x128, 4);" in create
    assert "BRIDGE_MEM8(w->object + 4) = 1;" in finish
    assert "BRIDGE_MEM32(w->object + 0x120) = w->exit_status;" in finish
    assert "object = worker->object;" in reference
    delay = body("static void bridge_KeDelayExecutionThread(")
    scheduler = body("static void worker_resume_if_due(")
    assert "MM3_ICALLS_PER_MS" not in delay
    assert "g_icall_count >= w->wake_icall" not in scheduler
