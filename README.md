# zmk-feature-runtime-tap-dance

Tap dance you can change from Keeb-On! Studio, without rebuilding firmware.

ZMK already ships a tap-dance behavior, and its state machine is the right
one — this module reuses it almost line for line. The one thing that differs
is where the bindings live. Upstream keeps them in the devicetree, so
changing what two taps do means a rebuild and a reflash. On a 30% keyboard,
where tap dance is how the missing keys come back, that is the wrong place
for the decision to live.

Here each slot's taps are **custom settings**, so the app edits them the same
way it edits any other setting. That is also why this module has no protobuf
and no RPC handler of its own: `zmk-feature-custom-settings` and its Studio
RPC already do that work.

## Using it

In `west.yml`:

```yaml
    - name: zmk-feature-runtime-tap-dance
      remote: salicylic-acid3
      revision: main
```

In your `.conf`:

```conf
CONFIG_ZMK_RUNTIME_TAP_DANCE=y
CONFIG_ZMK_RUNTIME_TAP_DANCE_COUNT=4
CONFIG_ZMK_RUNTIME_TAP_DANCE_MAX_TAPS=3
```

In your `.keymap`:

```c
#include <behaviors/runtime_tap_dance.dtsi>
```

then bind a slot by index:

```c
&rtd 0   // the first tap dance
&rtd 1   // the second
```

Every slot exists from the first boot and starts **empty**, which means
pressing it does nothing until you configure it in the app. That is
deliberate: a slot that did something before anyone chose what it should do
would be worse than one that does nothing.

## Why there is a Studio subsystem with no RPC

`src/studio/runtime_tap_dance_subsystem.c` registers this module as a Studio
custom subsystem and handles nothing. That is not vestigial.

A setting carries a `custom_subsystem_id` string, and the settings RPC turns
it into the index the app sees by looking it up in the registered Studio
subsystems. A setting whose id matches no registered subsystem cannot be given
an index, so it is dropped from `ListSettings` — silently, and from every
listing. Without that registration the slots exist on the keyboard and the app
simply cannot see them.

## What is stored

Per slot `N`, under the `keebon__runtime_tap_dance` subsystem:

| Key                | Type              | Meaning                              |
|--------------------|-------------------|--------------------------------------|
| `tap_danceN/taps`  | array of behavior | what 1 tap, 2 taps, … do             |
| `tap_danceN/term`  | int32 (50–1000)   | how long it waits for the next tap   |
| `max_taps`         | int32 (pinned)    | how many taps a slot may hold        |

The array's active length **is** the tap count: a slot with two entries is a
tap/double-tap, and the second tap fires immediately because there is nothing
longer to wait for.

### Why `max_taps` is a setting

An array setting's RPC value reports the array's *current* length, and the
protocol has no field for its capacity. So there is no way for the app to tell
a slot with room left from a full one — it would have to offer "add a tap"
until the firmware refused, turning a disabled button into an error message.
`max_taps` publishes the number instead. Its range constraint is
`(CONFIG_ZMK_RUNTIME_TAP_DANCE_MAX_TAPS, CONFIG_ZMK_RUNTIME_TAP_DANCE_MAX_TAPS)`,
so it can be read but not meaningfully written: the value is decided at build
time and a write of anything else is rejected.

### An empty slot lists nothing

List enumeration sends one notification per *active* array element, so a slot
whose taps array is empty — which is every slot on a freshly flashed keyboard
— contributes no `taps` setting at all. Its `term` is what tells the app the
slot exists, and a client appending the first tap has to name the array by
reference rather than by pointing at a listed setting.

## Deliberate omissions

**No `ignore-key-positions`.** Upstream takes a devicetree list of positions
that should not interrupt a dance. There is nowhere to put that yet which the
app can edit, so it is left out; the effect is upstream's behaviour with an
empty list — any other key press decides the dance.

**No hold-on-last-tap.** A tap dance here is taps only. Hold behaviour is
already available by binding a hold-tap *as* one of the taps, which composes
rather than duplicating.

## Cost

Per slot: one array of `MAX_TAPS` behavior values plus one int, in settings.
The `MAX_HELD` figure is separate — it bounds how many dances can be
mid-sequence at once, which is about fast typing across several tap-dance
keys rather than about how many slots exist.
