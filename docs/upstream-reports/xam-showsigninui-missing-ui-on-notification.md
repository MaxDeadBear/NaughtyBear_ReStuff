# rexglue SDK: `XamShowSigninUI` broadcasts `XN_SYS_UI(off)` with no preceding `XN_SYS_UI(on)`, hanging titles that gate on the overlay open→close transition

**Reported against:** rexglue SDK source tree (files/lines cited below re-verified against the checkout at the time of writing)
**Reporter context:** downstream static-recompilation project of a commercial Xbox 360 title (Naughty Bear), running the recompiled binary on a Linux x86-64 host through the SDK runtime.

---

## Summary

`XamShowSigninUI_entry` announces a system-UI *close* without ever announcing the matching *open*. A title that advances its sign-in state machine on the `XN_SYS_UI` **on→off transition** — the normal way to wait for a system overlay to be dismissed — never observes the transition, so it re-invokes `XamShowSigninUI` on a fixed retry interval forever and never leaves the sign-in prompt. To the player this is a hang: the "Would you like to sign in with a gamer profile?" prompt cannot be gotten past, and interacting with it only re-triggers the loop.

The SDK's own new-listener code path already emits the correct `on` **then** `off` pair for exactly this reason, so the fix is a one-line addition that makes `XamShowSigninUI` consistent with it.

---

## Environment

- Host: Linux x86-64 (glibc), SDK built from source.
- Guest: recompiled X360 PPC title. The title calls `XamShowSigninUI` from its front-end menu flow when no LIVE profile is signed in.
- Not host- or backend-specific: the defect is in the shared XAM layer, independent of graphics/threading backend.

---

## The defect

`src/kernel/xam/xam_user.cpp:492`:

```cpp
ppc_u32_result_t XamShowSigninUI_entry(ppc_u32_t unk, ppc_u32_t unk_mask) {
  // Mask values vary. Probably matching user types? Local/remote?

  // To fix game modes that display a 4 profile signin UI (even if playing
  // alone):
  // XN_SYS_SIGNINCHANGED
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, 1);
  // Games seem to sit and loop until we trigger this notification:
  // XN_SYS_UI (off)
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);
  return X_ERROR_SUCCESS;
}
```

It broadcasts `XN_SYS_SIGNINCHANGED` (`0x0000000A`) and then `XN_SYS_UI` (`0x00000009`) **with data `0` (off)**. There is no `XN_SYS_UI` with data `1` (on) beforehand. The in-code comment ("Games seem to sit and loop until we trigger this notification") shows the loop was observed, but the fix applied — a lone `off` — is incomplete for titles that gate on the transition rather than on a bare `off` edge.

That the *transition* is what titles consume is confirmed by the SDK's own listener-registration path, `src/system/kernel_state.cpp:796-798`, which enqueues the pair in order:

```cpp
    // XN_SYS_UI (on, off)
    listener->EnqueueNotification(0x00000009, 1);
    listener->EnqueueNotification(0x00000009, 0);
```

`BroadcastNotification` → `EnqueueNotification` (`src/system/kernel_state.cpp:821`) is a per-listener queue drained on the guest's own notification-polling thread, so an `on` immediately followed by an `off` are delivered as two distinct queue entries and read by the guest as a proper open-then-close transition — exactly what the registration path relies on, and exactly what `XamShowSigninUI` fails to provide.

---

## Observed downstream behavior

Instrumenting the title's guest-side sign-in gate (addresses are the downstream image's, given only to show the shape):

- The prompt gate returns "show the UI" while a UI-state object's mode field is 3 or 4 **and** its "done" field is still zero.
- The done field is set only when the guest observes the `XN_SYS_UI` on→off transition.
- With the SDK sending `off` alone, the done field never sets, so the gate keeps returning true and the title re-calls `XamShowSigninUI` on a ~3-second cadence indefinitely:

```
ShowSigninUI  t=77374ms
ShowSigninUI  t=80341ms
ShowSigninUI  t=83241ms
ShowSigninUI  t=86241ms   ... (unbounded)
```

The intervening `XamUserGetSigninState` calls all return "signed in" for user 0, so this is not a sign-in-state problem — the title is purely waiting on the UI transition.

The failure is intermittent in practice only because it depends on the title reaching the prompt, which is governed by front-end timing; when reached, it always hangs.

---

## Suggested fix

The natural first attempt — emit the missing `on` before the existing pair, matching `kernel_state.cpp:797-798`:

```cpp
ppc_u32_result_t XamShowSigninUI_entry(ppc_u32_t unk, ppc_u32_t unk_mask) {
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 1);  // XN_SYS_UI on
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, 1);  // XN_SYS_SIGNINCHANGED
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);  // XN_SYS_UI off
  return X_ERROR_SUCCESS;
}
```

**⚠️ Measured result (downstream): the ordered on/off pair alone does NOT unblock the Naughty Bear title.** A hook that broadcast the missing `on` before the SDK's pair fired on every stuck boot and the ~3-second `XamShowSigninUI` retry loop continued unchanged. So for this title the front-end is not merely waiting on the `XN_SYS_UI` transition — `XamShowSigninUI` being a no-op stub (it neither signs a user in nor produces a cancellable overlay whose result the title can read) leaves the title's sign-in screen with nothing to advance on, and it re-invokes the call indefinitely.

The report stands as an SDK-correctness issue — `XamShowSigninUI` returning `X_ERROR_SUCCESS` while doing nothing an actual sign-in UI would do is a latent hang for any title that loops on it — but the notification-ordering fix is not sufficient in general. A faithful implementation would model a real overlay lifetime: announce open, let the title observe it, then resolve as either "a user signed in" (broadcast `SIGNINCHANGED` and leave the signin state consistent) or "cancelled" (leave state unchanged), so the title's post-UI check has a definite result to proceed from.

**How the downstream actually fixed it:** rather than repair the stub, it removed the *reason the title reaches it*. The title's sign-in screen calls `XamShowSigninUI` only when it fails to find a signed-in user across the four pads; under host load a per-screen slot cache is transiently empty at render time, so no user is found and the loop starts. Forcing the slot-0 user check to report the (already-signed-in) user 0 keeps the screen on its normal "user present → skip prompt" path and the title never calls `XamShowSigninUI`. That is a title-specific workaround, not an SDK fix, but it is the reliable one; the SDK-level fix above remains desirable for titles that legitimately open the sign-in UI.

---

## Addendum (Aug 11): downstream evidence narrows this further

Two findings from the downstream title change what this report can claim.

**1. The signin state is not involved at all.** A trace captured from a boot that actually hung on
the prompt shows the title's `XamUserGetSigninState(0)` returning `1` on *every* call, including
throughout the hang (users 1-3 return 0, as expected for a single local user):

```
GetSigninState(user=0) -> 1   t=77406ms
GetSigninState(user=0) -> 1   t=80373ms
GetSigninState(user=0) -> 1   t=83273ms
```

There is no window in which the SDK reports "nobody signed in", so any theory that the title prompts
because it transiently sees an unsigned-in user is wrong for this title. The report's original claim
— that the title is "purely waiting on the UI transition" — is consistent with this.

**2. The retry loop is driven from the title's notification pump, and the prompt itself is a
title-side screen, not the system blade.** The repeating polls come from a function that calls
`XNotifyGetNext` and switches on the notification id, handling `XN_SYS_UI` (9) and
`XN_SYS_SIGNINCHANGED` (10). The dialog the player sees is drawn by the title in its own art style
with "Sign In" / "Continue Without Sign In" options — it is the title's own state, which the title
raises and which then invokes `XamShowSigninUI`.

This means the practical impact of the `XamShowSigninUI` defect is narrower than first described: the
missing `XN_SYS_UI(on)` does not by itself cause the title to *raise* the prompt, it causes the title
to never resolve the prompt once raised. That still matches the reported symptom (the prompt cannot
be got past, and interacting with it re-triggers the loop), and the suggested fix — modelling a real
overlay lifetime, announce open, then resolve as signed-in or cancelled — remains the right one.
What should be dropped is any implication that the notification ordering alone determines whether the
prompt appears; measured downstream, emitting the ordered on/off pair did not stop the loop.
