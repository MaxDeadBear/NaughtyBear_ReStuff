# rexglue SDK: a second controller is reported CONNECTED on user_index 1 with no physical device attached, making titles prompt for a gamer profile

**Reported against:** rexglue SDK 0.8.1 (linux-amd64), used by a downstream static-recompilation
project of a commercial Xbox 360 title (Naughty Bear), running headless on Linux x86-64.

---

## Summary

`XamInputGetState` reports `ERROR_SUCCESS` (i.e. *device connected*) for **user_index 1** on a machine
with **no controllers attached at all**. Slots 2 and 3 correctly return
`ERROR_DEVICE_NOT_CONNECTED (0x48F)`. Because `XamUserGetSigninState` reports a signed-in user for
slot 0 only, the guest observes **a connected controller with no profile attached on slot 1**, which
is precisely the condition a title is supposed to react to by asking the player to sign in.

In the downstream title this surfaces as a "Would you like to sign in with a gamer profile?" screen
that the player cannot get past, because the title's follow-up call lands in the `XamShowSigninUI`
stub (reported separately). But the sign-in screen is a *correct* response to what the runtime is
telling it — the defect is the phantom device.

---

## Evidence

Instrumenting the guest's `XamInputGetState` wrapper and logging the return code per slot, on a
headless run with no controller of any kind connected:

```
user=0 -> 00000000   ERROR_SUCCESS               (connected)
user=1 -> 00000000   ERROR_SUCCESS               (connected)   <-- no such device exists
user=2 -> 0000048F   ERROR_DEVICE_NOT_CONNECTED
user=3 -> 0000048F   ERROR_DEVICE_NOT_CONNECTED
```

That pattern is stable across runs and appears from the very first poll (process-relative t=0ms).

The guest then behaves exactly as the API contract implies. Its per-slot profile query
(a function taking the slot index and bailing unless that slot's signin state is 1 or 2) is invoked
for slot 1 and gets 0:

```
healthy boot: GetSigninState(user=0) -> 1   at t=181ms
stuck boot:   GetSigninState(user=0) -> 1   at t=183ms
              GetSigninState(user=1) -> 0   at t=18816ms   <-- only on boots that then prompt
```

Boots that never make that slot-1 query never raise the prompt. So the phantom slot is not merely
cosmetic: it changes control flow in the title.

---

## Where it appears to come from

`rex/input/input_system.h` documents the default configuration:

```cpp
/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);
```

`InputSystem::GetState(user_index, ...)` iterates `drivers_` and (following the Xenia design this is
derived from) returns the first driver that does not answer `ERROR_DEVICE_NOT_CONNECTED`. With two
drivers registered, one of them is answering `ERROR_SUCCESS` for slot 1 when no device backs it —
either a driver that reports success for a slot it does not actually own, or one whose
"no device" path returns success with a zeroed `X_INPUT_STATE`.

Note the asymmetry that makes this a bug rather than a policy choice: slots 2 and 3 *do* return
`ERROR_DEVICE_NOT_CONNECTED`, so whatever produces slot 1 is not applying the same rule to all
unbacked slots.

---

## Impact

Any title that enumerates controllers and checks each one's profile — a very common pattern for
"press START to play" front-ends and for local-multiplayer join flows — will see a player that does
not exist. Depending on the title that means a sign-in prompt, a spurious second-player slot, or a
join flow that cannot complete. It is invisible to titles that only ever read slot 0.

---

## Suggested fix

`XamInputGetState` (and `XamInputGetCapabilities`, which titles use the same way) should return
`ERROR_DEVICE_NOT_CONNECTED` for any slot with no device actually bound to it, so that slot 1 behaves
like slots 2 and 3. Concretely, whichever driver currently answers for slot 1 should either decline
slots it does not own, or map its single logical device onto exactly one slot.

## Downstream workaround (not a fix)

Until the input layer is corrected, the downstream project reports the local user as *signed in* for
any slot the runtime claims is connected, so a phantom slot at least carries a consistent profile.
That removes the prompt without suppressing input, and it is correct either way — if the slot is
phantom it never sends input, and if a real second controller is present it belongs to the same
local user. It is a workaround for a runtime that is describing hardware that is not there.
