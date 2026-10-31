/* Calls the input entry points a game uses, so the linker keeps everything
 * xbox_input pulls in. The import check then reads the exe's import table:
 * no XInput DLL may be among them. Run on Windows it only has to start (with
 * no pad connected the calls just fail). */
#include <windows.h>
#include "xinput_xbox.h"

int main(void)
{
    XBOX_INPUT_STATE state;
    XBOX_VIBRATION vibration = {0};
    XBOX_INPUT_CAPABILITIES caps;
    DWORD rc;
    xbox_InputInit();
    rc = xbox_InputGetState(0, &state);
    rc ^= xbox_InputSetState(0, &vibration);
    rc ^= xbox_InputGetCapabilities(0, 0, &caps);
    return (int)rc;
}
