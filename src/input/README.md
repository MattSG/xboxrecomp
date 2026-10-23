# Host gamepad and keyboard integration

[xinput_device.c](xinput_device.c) converts host input into the types declared
in [xinput_xbox.h](xinput_xbox.h). Windows uses XInput; other hosts use SDL3
gamepads. Xbox digital flags, eight analog-button bytes and signed stick axes
remain separate. This host facade does not replace the guest XAPI or USB device
model used by a port.

## Host API

Call `xbox_InputInit` before polling. `xbox_InputGetState` accepts ports 0-3 and
returns zero on success. Check that result before reading the output.
`xbox_InputSetState` sends motor values to the host backend;
`xbox_InputGetCapabilities` exposes the implemented subset of capabilities.
`xbox_InputIsConnected` reports cached connection state rather than polling the
host. Backend behavior and capabilities are partial, not hardware parity.

By default, face buttons become 0/255 pressure values, right shoulder is Black,
left shoulder is White, and trigger axes remain analog. `RECOMP_PADMAP` accepts
`NAME:SOURCE,...` assignments such as `BLACK:RB,WHITE:LB`. Input names are A, B,
X, Y, BLACK, WHITE, START, BACK, LS, RS, UP, DOWN, LEFT, RIGHT, LT and RT;
source names use LB/RB for the shoulders. Unspecified bindings retain defaults.

## Keyboard and menu callbacks

`RECOMP_KEYBOARD=1` enables an optional keyboard pad on port 0, merged with a
physical pad when present. `RECOMP_KEYMAP` accepts `NAME:VK,...` bindings with
hexadecimal virtual-key values; zero disables a binding. For example,
`START:0,A:57` disables keyboard Start and binds A to W. Defaults and supported
axis names are defined alongside the parser in the implementation.

`RECOMP_MENU_KEYS=1` adds navigation/confirm/back keys while the registered menu
probe returns nonzero; without a probe it applies continuously. Ports register
context with `xbox_InputSetMenuProbe`, contextual keyboard behavior with
`xbox_InputSetKeyboardHook`, and optional click consumption with
`xbox_InputSetMenuClickHandler`. NULL clears each callback. Configure before
input starts. Keep captured menu state, application actions and guest-thread
handoff in the port.

The keyboard hook receives the keyboard state after default mapping and before
physical-pad merging. Menu click consumption can be invoked from framebuffer
mouse dispatch as well as polling; callbacks must be safe for their calling
threads and queue guest work when needed. Returning nonzero consumes a click;
otherwise menu input can use the bounded mouse-to-A fallback. Host input capture
neutralizes polling output and advances packet state.

## Verification and limits

[Keymap checks](../../tests/keymap/CMakeLists.txt) exercise remapping, contextual
hooks, click consumption/fallback and default shoulder mapping; the SDL fixture
also checks consecutive discovery of two virtual pads. These focused checks do
not establish all hotplug, concurrency, capture, vibration or device semantics.
Environment mappings are cached by the implementation; treat configuration as
startup policy. Memory units and accessories require their own implementation.
