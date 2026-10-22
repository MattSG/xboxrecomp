# Evaluating a recompilation target

Measure the executable and required runtime behavior before estimating port
work. Generated code or reaching a guest entry point does not establish
playable menus, loading or gameplay.

## Inspect the executable

From the toolkit root:

```sh
python -m tools.xbe_parser path/to/default.xbe --json output/analysis.json
```

Record sections, entry point, kernel imports, library version stamps and
demand-loaded sections. Follow the [XBE parsing guide](../pipeline/01-xbe-parsing.md)
and [disassembly guide](../pipeline/02-disassembly.md) to inspect code boundaries,
indirect-call targets, switch tables and unresolved instructions. Measure the
recovered functions instead of estimating them from bytes per function.

## Compare requirements with the runtime

- Check required kernel behavior, not just whether an export has a symbol.
- Identify thread, synchronization, exception and dynamic-code requirements.
- Trace graphics submission, surface formats, audio/DSP, video, filesystem,
  saving and network dependencies reached by the intended mode.
- Use engine/library familiarity as a hypothesis; verify layouts, calling
  conventions and behavior against this executable.

See [runtime integration](../runtime/README.md) for shared interfaces. Keep
guest-address overrides, authored assets and gameplay/rendering policy in the
port. Fix reusable lifter/runtime mechanisms here and expose a generic hook
when a port needs to supply policy.

## Establish acceptance incrementally

Record separate evidence for boot, menus, loading, player movement, rendering,
audio and saving. Inspect gameplay captures and telemetry for the intended
mode; a generated-code percentage or recognizable static frame is insufficient.
Keep original disc data intact and use isolated saves for experiments.
