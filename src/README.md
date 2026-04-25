# Source Layout

`src` is organized by responsibility so plugin entry points, UI, services, and
shared domain code stay separate.

## `core`

Shared plugin infrastructure and domain logic:

- BG3 localization content detection and cache management
- PAK reading
- Divine/LSLib discovery
- plugin logging

Code in this folder should not depend on Qt widgets or plugin entry-point types.

## `services`

External integrations and long-running clients:

- Gemini translation client
- Nexus Mods discovery API

Services may depend on `core`, but they should not construct UI.

## `ui`

Qt widgets, dialogs, tabs, delegates, and UI-specific orchestration.

UI classes may call `core` and `services`, but domain logic should be pushed down
instead of growing inside widgets.

## `plugins`

MO2 plugin entry points and lifecycle glue.

Plugin classes wire together `core`, `services`, and `ui` objects. Keep business
logic out of this layer where possible.
