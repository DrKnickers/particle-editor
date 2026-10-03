# Find the web interface code

Start with [CONTRIBUTING.md](../../../CONTRIBUTING.md#start-here) to run the
interface in a browser and make a first label change. React builds the visible
interface from components, small pieces of screen code. The **bridge** lets
those pieces ask the Windows **host** to read or change native state. In a
browser, a **mock** supplies sample data instead of the host.

See [How the pieces fit](../../../src/README.md#how-the-pieces-fit) for a
picture of the interface, host, engine and file paths.

## Where to look

| Folder | What it is for; when to open it |
|---|---|
| `src/screens` | Whole panels and dialogs. Start with [EmitterPropertyTabs.tsx](src/screens/EmitterPropertyTabs.tsx) for the inspector, then [BasicTab.tsx](src/screens/property-tabs/BasicTab.tsx), [AppearanceTab.tsx](src/screens/property-tabs/AppearanceTab.tsx) or [PhysicsTab.tsx](src/screens/property-tabs/PhysicsTab.tsx) for a field. For the tree, open [EmitterTree.tsx](src/screens/EmitterTree.tsx), [EmitterRow.tsx](src/screens/emitter-tree/EmitterRow.tsx) or [EmitterTreeToolbar.tsx](src/screens/emitter-tree/EmitterTreeToolbar.tsx). Curves live in [CurveEditorPanel.tsx](src/screens/curve-editor/CurveEditorPanel.tsx) and [CurveEditor.tsx](src/screens/curve-editor/CurveEditor.tsx). |
| `src/components` | Pieces shared around the main screen. Open [PanelLayout.tsx](src/components/PanelLayout.tsx) for panel placement, [MenuBar.tsx](src/components/MenuBar.tsx) for menus, or [Toolbar.tsx](src/components/Toolbar.tsx) for toolbar buttons. |
| `src/primitives` | Basic controls. Open [Spinner.tsx](src/primitives/Spinner.tsx) for number entry, [Select.tsx](src/primitives/Select.tsx) for a list of choices, or [Tip.tsx](src/primitives/Tip.tsx) for hover help. A spinner is a number box with step buttons. |
| `src/lib` | Shared rules and state. Open [file-state.ts](src/lib/file-state.ts) for file status and save prompts, [emitter-selection.ts](src/lib/tree/emitter-selection.ts) for multiple selection, or [curve-model.ts](src/lib/curve/curve-model.ts) for curve calculations. |
| `src/bridge` | Host communication and the browser stand-in. [index.ts](src/bridge/index.ts), `makeBridge`, chooses [native.ts](src/bridge/native.ts) inside WebView2, the embedded browser, and [mock.ts](src/bridge/mock.ts) outside it. The mock routes requests to `src/bridge/mock-dispatch/` files matching the ten native handler groups. Open these when a request or event behaves differently in the two modes. |
| `src/styles` | [tokens.css](src/styles/tokens.css) holds shared colours and sizes. [components.css](src/styles/components.css) holds rules for controls and panels. [globals.css](src/styles/globals.css) sets page-wide rules. Open these for appearance changes. |

[App.tsx](src/App.tsx) connects the main pieces. The
[bridge schema](../../packages/bridge-schema/src/index.ts) defines request,
answer and event shapes. Search for the request name, such as
`emitters/set-track-key`. Find its native handler in the
[native source map](../../../src/README.md#find-a-request-handler).

## Who owns this state?

The owner holds the value that other parts must follow. A **cache** is a local
copy for reading. An **event** is a message the host sends when something
changes. **Persistence** means keeping a value after closing the program.
**Undo** means restoring a saved earlier editing state.
A **snapshot** is a copy of the current values.

| State | Owner | How it changes | Persistence | Undo | Readers | Source to open |
|---|---|---|---|---|---|---|
| Document: emitter definitions and curves | Host's `ParticleSystem` | Property, track and tree requests change native fields; events tell the web to read again. | Written to `.alo` on Save. | Native snapshots restore document content. | Property tabs, tree, curve panel and particle engine. | [ParticleSystem.h](../../../src/ParticleSystem.h), [UndoStack.cpp](../../../src/UndoStack.cpp), [dispatch map](../../../src/README.md#find-a-request-handler). |
| File path, dirty flag and recent files | Host; web keeps a copy. Dirty means there are unsaved changes. | File requests update path and recents; edits set dirty. `useSeedFileState` reads snapshots and follows events. | Content goes in `.alo`; recent paths go in Windows settings. Current path and dirty flag are session state. | Restoring content recalculates dirty against the last saved snapshot. It does not restore the file path or recent list. | App title, File menu and save prompt. | [file-state.ts](src/lib/file-state.ts), [BridgeDispatch_File.cpp](../../../src/host/BridgeDispatch_File.cpp), [BridgeDispatcher.cpp](../../../src/host/BridgeDispatcher.cpp), `ApplyUndoSnapshot`. |
| Primary emitter selection: one row | Host's `m_selectedEmitterId` | `emitters/select` sets it; `emitters/selected` and engine events report it. | Session only; absent from `.alo`. | Selection alone creates no undo step. Document snapshots capture and restore the selected position. | Inspector, curve panel and tree. | [BridgeDispatch_Emitters.cpp](../../../src/host/BridgeDispatch_Emitters.cpp), [BridgeDispatcher.cpp](../../../src/host/BridgeDispatcher.cpp), `CaptureUndoPoint`. |
| Multiple emitter selection | Web's `useEmitterSelectionStore` | Tree gestures call `setSingle`, `toggle`, `range` or `setIds`. The tree sends only the primary row to the host; batch actions send their own list of IDs. | Memory only. | No history for the set. Host selection events can adjust it after document undo. | Tree, keyboard navigation and batch menu actions. | [emitter-selection.ts](src/lib/tree/emitter-selection.ts), [EmitterTree.tsx](src/screens/EmitterTree.tsx), [MenuBar.tsx](src/components/MenuBar.tsx). |
| Engine snapshot cache | Host owns values; `useEngineField` keeps one cache per bridge. | Starts with `engine/state/snapshot`, then follows `engine/state/changed`. Stops listening when the last reader leaves. | Cache stays in memory; this hook saves nothing. Individual fields have their own owners. | Cache has no undo history; it follows host events. | Toolbar, MenuBar, StatusBar and viewport controls. | [use-engine-snapshot.ts](src/lib/use-engine-snapshot.ts). |
| Right dock: which side pane is open | Web store | `toggleDock` and `setDock` change it. | Browser `localStorage`, a saved key/value store, under `alo:right-dock`. Atlas reopens closed because it needs a selection. | No undo history. | PanelLayout, Toolbar, View menu and pane close buttons. | [right-dock.ts](src/lib/right-dock.ts), [PanelLayout.tsx](src/components/PanelLayout.tsx). |
| Theme choice | Web | Preferences calls `applyMode`; App follows the operating system when the choice is `system`. | Browser storage under `alo:theme`. | No undo history. | Page colour rules and Preferences. | [theme.ts](src/lib/theme.ts), [stored-pref.ts](src/lib/stored-pref.ts), [App.tsx](src/App.tsx). |
| Lighting | Host engine holds live lights; host settings hold the saved colour/intensity split. LightingPane keeps form values. | `engine/set/light`, `engine/set/ambient` and `engine/set/shadow` update the preview. Separate `settings/lighting/set` saves form values; `settings/lighting` reads them. | Windows registry, a settings store, through `LightingValues`; restored at startup. Not part of `.alo`. Test-host mode normally disables live settings reads and writes. | These lighting requests capture no undo point. The preview setters still mark the document dirty. | LightingPane and engine rendering. | [LightingPane.tsx](src/screens/LightingPane.tsx), [BridgeDispatch_Engine.cpp](../../../src/host/BridgeDispatch_Engine.cpp), [BridgeDispatch_SpawnerLighting.cpp](../../../src/host/BridgeDispatch_SpawnerLighting.cpp), [RestoredSettings.cpp](../../../src/host/RestoredSettings.cpp). |

## How one edit travels

This example changes **Initial spawn delay** (`initialDelay`), measured in
seconds. It shows where to look when a field changes on screen but does not
reach the preview or saved file.

1. In [BasicTab.tsx](src/screens/property-tabs/BasicTab.tsx), search for
   `Initial spawn delay:`. Its `FieldSpinner` receives `properties.initialDelay`
   and commits `{ initialDelay: v }`. It displays two decimal places and
   has a step of 0.1 seconds.
2. [fields.tsx](src/screens/property-tabs/fields.tsx), `FieldSpinner`, passes
   that value and callback to [Spinner.tsx](src/primitives/Spinner.tsx).
   Typing commits on blur; Enter causes blur. Step, wheel and drag actions
   also call `onChange`. `min={0}` clamps edits to zero or above. It does not
   repair a negative value received from the host.
3. [EmitterPropertyTabs.tsx](src/screens/EmitterPropertyTabs.tsx), `commit`,
   updates the form immediately and sends
   `emitters/set-properties` with `params: { id: selectedId, patch }`.
   The patch contains the changed `initialDelay` value from
   [BasicTab.tsx](src/screens/property-tabs/BasicTab.tsx).
   A patch is a group of fields to change. This immediate local update is
   called optimistic: the host has not answered yet.
4. [NativeBridge](src/bridge/native.ts), `request`, puts the request in a
   JSON message and sends it through WebView2. JSON is the text format used
   for bridge messages. [BridgeDispatcher.cpp](../../../src/host/BridgeDispatcher.cpp),
   `DispatchInternal`, routes it to
   [BridgeDispatch_EmitterProperties.cpp](../../../src/host/BridgeDispatch_EmitterProperties.cpp).
5. Search that handler for `emitters/set-properties`. It rejects an unknown
   emitter or a missing/non-object patch. Before changing fields, it calls
   `captureUndo`. Rapid edits with the same emitter and field names can share
   one undo step. `getFloat` assigns the JSON number to `emit->initialDelay`.
6. The handler calls `propagateLinkGroup`. A link group shares settings
   between emitters. [BridgeDispatcher.cpp](../../../src/host/BridgeDispatcher.cpp)
   copies shared fields to other members;
   [ParticleSystem.cpp](../../../src/ParticleSystem.cpp), `copySharedParamsFrom`,
   preserves `initialDelay` on a member when that field is exempt. The undo
   snapshot covers the whole group.
7. [BridgeDispatch_EmitterProperties.cpp](../../../src/host/BridgeDispatch_EmitterProperties.cpp)
   returns `applied` and `skipped` field names, calls `MarkDirty`,
   then calls `OnParticleSystemChanged(-1)` when an engine is bound. This
   refreshes live instances and allows a paused preview to repaint. It emits
   `engine/state/changed` and `emitters/tree/changed`. `MarkDirty` also emits
   `dirty/changed` if the dirty flag changed; see
   [BridgeDispatcher.cpp](../../../src/host/BridgeDispatcher.cpp). Refreshing does not promise to
   restart an existing instance's initial wait; see
   [EmitterInstance.cpp](../../../src/EmitterInstance.cpp), `onParticleSystemChanged`,
   and [SpawnSchedule.h](../../../src/SpawnSchedule.h), `ReconcileNextSpawnTime`.
8. The tree event makes
   [EmitterPropertyTabs.tsx](src/screens/EmitterPropertyTabs.tsx) call `fetchProps` for the
   current selection. [tree-refetch.ts](src/lib/tree-refetch.ts),
   `requestTreeRefetch`, shares matching reads made in the same turn.
   [BridgeDispatch_EmitterProperties.cpp](../../../src/host/BridgeDispatch_EmitterProperties.cpp),
   `emitters/get-properties`, returns the native `initialDelay`, and the form
   replaces its local copy. A rejected edit is announced in the status bar
   and also calls `fetchProps`.
9. The edit itself does not save a file. On Save,
   [BridgeDispatch_File.cpp](../../../src/host/BridgeDispatch_File.cpp),
   `file/save`, calls [ParticleSystemIO.cpp](../../../src/ParticleSystemIO.cpp),
   `SaveParticleSystem`. [AtomicSave.cpp](../../../src/AtomicSave.cpp),
   `AtomicWriteParticleSystem`, writes a temporary file through
   `ParticleSystem::write` and replaces the destination after a successful
   write. [ParticleSystemSerialization.cpp](../../../src/ParticleSystemSerialization.cpp),
   `Emitter::writeProperties`, writes `initialDelay` as a float, a number that
   can have a fractional part. It goes in mini-chunk `0x24`, a numbered field
   in the file. `readProperties` reads it back.

### Where validation happens

The spinner's zero minimum is an interface rule. Native `getFloat` checks
that `initialDelay` is a JSON number; it does **not** enforce that minimum.
A wrong type keeps the old value and lists the field in `skipped`. Unknown
field names are also skipped. Even a skipped patch reaches the handler's
dirty, refresh and event code. The web `commit` reads the answer: when
`skipped` is not empty it announces the skipped field names in the status
bar and reads the properties again, so the form shows the host's value.

[dispatch-emitter-properties.ts](src/bridge/mock-dispatch/dispatch-emitter-properties.ts), `emitters/set-properties`, is more lenient.
It accepts known fields without native type checks, including the derived
`blendAlphaGated` field, which native skips. An unknown emitter returns two
empty lists instead of rejecting. Browser checks therefore cannot prove
native validation, rendering or file saving.

Use the **Property behaviour** row in
[Which check for which change](../../../CONTRIBUTING.md#which-check-for-which-change)
after changing a field. For a request addition, follow the checklist at the
top of the [bridge schema](../../packages/bridge-schema/src/index.ts).

## Adding a property field

Use **Gravity acceleration** as the example for a new emitter setting.
Follow its field through these seven steps so the browser, host and saved
file agree.

1. In the [bridge schema](../../packages/bridge-schema/src/index.ts), search
   for `gravity: number` in `EmitterPropertiesDto`. Add the new field with
   the type both sides will send.
2. In [mock-state.ts](src/bridge/mock-state.ts), search for
   `makeFixtureProperties` and `gravity: 0`. Add the browser default there.
   [mock.ts](src/bridge/mock.ts), `emitters/set-properties`, needs no new
   field-specific code: it derives `applied` and `skipped` from the fixture's
   keys. The mock does not check types; the host does.
3. In [ParticleSystem.h](../../../src/ParticleSystem.h), search for
   `float gravity` and add the stored member. In
   [ParticleSystem.cpp](../../../src/ParticleSystem.cpp), search for
   `setDefaults` and give it a default. In
   [ParticleSystemSerialization.cpp](../../../src/ParticleSystemSerialization.cpp),
   search for `writeMiniFloat  (writer, 0x0C, gravity)` for the write line,
   then `case 0x0C:` and `readFloat(reader)` for the matching read line.
   Add both sides for the new field. Mini-chunk IDs must be unique within
   chunk `0x0002`; read `writeProperties` to find the next free ID rather
   than guessing from the gravity ID.
4. In [BridgeDispatch_EmitterProperties.cpp](../../../src/host/BridgeDispatch_EmitterProperties.cpp),
   search for `"gravity"`. Add one line to the get response and one
   `patch.contains` line to the set handler. Choose the matching reader:
   `getFloat`, `getInt`, `getBool` or `getString`. These helpers record
   accepted fields in `applied` and wrong types in `skipped`, keeping the
   old value when a type is wrong.
5. In [PhysicsTab.tsx](src/screens/property-tabs/PhysicsTab.tsx), search for
   `Gravity acceleration:` to see the field's value and commit callback.
   Use `FieldSpinner`, `FieldCheckbox` or `FieldSelect` from
   [fields.tsx](src/screens/property-tabs/fields.tsx) in the appropriate tab.
   Add `help` when an explanation is useful. A reason for disabling a field
   appears only while it is disabled; a plain explanatory sentence can
   remain visible. Follow the existing field's bounds and units only when
   they also apply to the new setting.
6. If the field should be shared in a link group, add its exemption flag in
   [LinkGroup.h](../../../src/LinkGroup.h), `bool gravity`, and its default
   and difference check in [LinkGroup.cpp](../../../src/LinkGroup.cpp),
   `gravity(false)` and `CHECK_FIELD(gravity,`. In
   [ParticleSystem.cpp](../../../src/ParticleSystem.cpp),
   `copySharedParamsFrom`, preserve and restore the field when exempt;
   search for `sav_gravity` for the example. Add the bridge flag mapping to
   [BridgeDispatcher.cpp](../../../src/host/BridgeDispatcher.cpp),
   `kLinkFieldTable`, and the label and group entries to
   [LinkGroupSettingsDialog.tsx](src/screens/LinkGroupSettingsDialog.tsx),
   `"Gravity"` and `"gravity"`. If the field should change when the user
   rescales an effect, add the appropriate time or size rule to
   [Rescale.cpp](../../../src/Rescale.cpp), `emitter->gravity`.
7. Extend the get/set round trip in
   [bridge-contract.emitters.test.ts](src/bridge/__tests__/bridge-contract.emitters.test.ts),
   `emitters/set-properties applies a partial patch`, and the label check
   in [EmitterPropertyTabs.test.tsx](src/screens/__tests__/EmitterPropertyTabs.test.tsx),
   `Gravity acceleration:`. Add a native round trip following
   [property-tabs.spec.ts](tests/property-tabs.spec.ts),
   `changing gravity round-trips via get-properties`. For the saved file,
   extend [test_alo_roundtrip.cpp](../../../tests/test_alo_roundtrip.cpp),
   `buildOne` and `ROUND-TRIP FIDELITY`, with a non-default value and an
   assertion that it survives writing and reading.

Use the **Property behaviour** row in
[Which check for which change](../../../CONTRIBUTING.md#which-check-for-which-change)
for the interface and bridge checks, and the **Native logic** row for
stored fields and file-format changes. A browser round trip proves the
mock path; native and file round trips check the real host and persistence.
