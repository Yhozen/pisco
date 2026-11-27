# Sessions, and what "the app works" means

A **session** is what a player does, written down: keys, clicks, mouse motion, waits, and where to take a
screenshot. The same file drives the oracle (the platform's real system: Wine, or QEMU) and a PISCO host, so that
what the two show can be compared. A session describes the player, never a host.

Sessions are JSON, in a recipe's `sessions/` folder.

```json
{ "schema": 1, "app": "narbacular-drop", "about": "the menu, then the first level",
  "steps": [ {"wait": 40000}, {"focus": true}, {"shot": "menu"},
             {"move": [-208, -95], "ms": 600}, {"click": "left"}, {"key": "w", "ms": 2000} ] }
```

## The keys

<!-- session-keys -->
| Key | Type | Required | Means |
|---|---|---|---|
| `schema` | integer | yes | 1 |
| `app` | string | yes | the `name` of the app it is for |
| `about` | string | no | for a person: what it does |
| `steps` | list | yes | the steps, played in order |
| `random` | list of strings | no | the values the app's random source answers first (hex), so that a random world is the oracle run's |
<!-- /session-keys -->

A reader must refuse a session whose schema it does not know, a key not in this table, and a step not in the next.

## Steps

Each step is an object with one of these keys, and the others its row allows:

<!-- session-steps -->
| Step | With | Means |
|---|---|---|
| `wait` | | let this many milliseconds of the app's time pass |
| `focus` | | give the app the input focus (`true`) |
| `move` | `ms` | move the mouse by `[dx, dy]`, spread over `ms` |
| `click` | `ms` | press and release `left` or `right` (held for `ms`) |
| `button` | `down` | press (`down: true`) or release a mouse button only |
| `key` | `ms`, `down` | a key by its X keysym name (or `space`, `enter`, `esc`, `up`, ...): pressed and released (held for `ms`), or with `down`, pressed or released only |
| `shot` | | a checkpoint: take a screenshot named so, and note the frame |
<!-- /session-steps -->

## Time

On the oracle, time is the wall's. On a host, a session should be played on a **virtual clock**: the app's clock
starts at a fixed date and moves by a fixed step when a frame is presented and by what it asks for when it sleeps,
so that a `wait` counts frames and two runs are the same. A host must say which clock a run used.

## Levels

What it means for an app to work, from the least to the most:

| Level | Holds when | Checked by |
|---|---|---|
| **builds** | the recipe's inputs give the app's files, and their tree digest is the lock's ([recipe.md](recipe.md#the-lock)) | the builder |
| **starts** | the app reaches its first frame (or, without a screen, prints what it prints on the oracle), with nothing it imports left unanswered | the app's start session, or its trace |
| **plays** | a session's checkpoints are reached at their frames, and its screenshots are close to the oracle's (the thresholds are the app's, beside its sessions) | the app's sessions against the oracle's run |

A recipe says how far it has been checked in [docs/status.md](../status.md#levels) (one row a recipe). An app's
level is the platform's oracle's: Wine for the Windows platforms, QEMU for the machine, the same program run on the
Linux that built it for a Linux app.
